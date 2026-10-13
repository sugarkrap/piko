#include "remote.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include <strings.h>

#include "pirdevicetypenames.h"
#include "pirinfraredled.h"
#include "pirkeysetmetadata.h"
#include "pirmakenames.h"
#include "pirselectdeviceform.h"
#include "protocols/pirprotocol.h"

PIRMakeMgr makeManager;
bool commandInFlight = false;
QMutex commandIFMutex;
bool stopRepeatingFlag = false;
QMutex stopRepeatingMutex;

void piko_keysets(std::vector<PIRKeysetMetaData *> &out);

namespace {

struct Access : PIRKeysetMetaData {
    static PIRProtocol *protocol(PIRKeysetMetaData *k)
    {
        return k->*(&Access::threadableProtocol);
    }
};

std::vector<PIRKeysetMetaData *> g_meta;
std::map<unsigned int, PIRKeysetMetaData *> g_by_id;
std::vector<RemoteKeyset> g_keysets;
QObject g_gui;
PIRKeysetMetaData *g_current;
void (*g_wake)(void);

std::mutex g_lock;
std::condition_variable g_cv;
PIRProtocol *g_job;
unsigned int g_job_id;
PIRKeyName g_job_key;
bool g_running;
bool g_held;
std::string g_error;

void report(const QString &s)
{
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_error = s.toStdString();
    }
    if (g_wake)
        g_wake();
}

void worker()
{
    for (;;) {
        PIRProtocol *job;
        unsigned int id;
        PIRKeyName key;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] { return g_job != NULL; });
            job = g_job;
            id = g_job_id;
            key = g_job_key;
            g_job = NULL;
            g_running = true;
        }
        g_cv.notify_all();

        job->startSendingCommand(id, key);

        {
            std::lock_guard<std::mutex> l(g_lock);
            g_running = false;
        }
        g_cv.notify_all();
        if (g_wake)
            g_wake();
    }
}

void raise_stop()
{
    QMutexLocker l(&stopRepeatingMutex);
    stopRepeatingFlag = true;
}

void stopper()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] { return g_running && !g_held; });
        }
        raise_stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void wait_idle()
{
    remote::release();
    std::unique_lock<std::mutex> l(g_lock);
    g_cv.wait(l, [] { return g_job == NULL && !g_running; });
}

bool keyset_less(const RemoteKeyset &a, const RemoteKeyset &b)
{
    int c = strcasecmp(a.make.c_str(), b.make.c_str());

    if (c)
        return c < 0;
    return a.id < b.id;
}

}

void PIRProtocol::errorMessage(QString s)
{
    report(s);
}

void PIRInfraredLED::errorMessage(QString s)
{
    report(s);
}

void PIRKeysetMetaData::errorMessage(QString s)
{
    report(s);
}

namespace remote {

void load(void (*wake)(void))
{
    g_wake = wake;
    piko_keysets(g_meta);

    for (size_t i = 0; i < g_meta.size(); i++) {
        PIRKeysetMetaData *m = g_meta[i];
        RemoteKeyset k;

        k.id = m->getID();
        k.make = makeManager.getMakeString(m->getMake());
        k.name = m->getKeysetName();
        k.panels = m->getPanelTypes();
        g_keysets.push_back(k);
        g_by_id[k.id] = m;
    }

    std::stable_sort(g_keysets.begin(), g_keysets.end(), keyset_less);

    std::thread(worker).detach();
    std::thread(stopper).detach();
}

const std::vector<RemoteKeyset> &keysets()
{
    return g_keysets;
}

const RemoteKeyset *find(unsigned int id)
{
    for (size_t i = 0; i < g_keysets.size(); i++)
        if (g_keysets[i].id == id)
            return &g_keysets[i];
    return NULL;
}

const RemoteKeyset *find(const std::string &make, const std::string &name)
{
    for (size_t i = 0; i < g_keysets.size(); i++)
        if (g_keysets[i].make == make && g_keysets[i].name == name)
            return &g_keysets[i];
    return NULL;
}

std::vector<std::string> makes()
{
    std::vector<std::string> out;

    for (size_t i = 0; i < g_keysets.size(); i++)
        if (out.empty() || out.back() != g_keysets[i].make)
            out.push_back(g_keysets[i].make);
    return out;
}

std::vector<RemoteDevice> devices()
{
    PIRSelectDeviceForm form;
    PIRDeviceTypeMgr types;
    std::vector<RemoteDevice> out;

    PIRKeysetMetaData::populateDevices(&form);

    for (size_t i = 0; i < form.items.size(); i++) {
        PIRKeysetWidgetItem *it = form.items[i];
        RemoteDevice d;

        d.make = makeManager.getMakeString(it->make);
        d.model = it->internalName.toStdString();
        d.type = types.getDeviceString(it->type);
        d.keyset = it->id;
        out.push_back(d);
    }
    return out;
}

bool select(unsigned int id)
{
    std::map<unsigned int, PIRKeysetMetaData *>::iterator it = g_by_id.find(id);

    if (it == g_by_id.end())
        return false;
    if (it->second == g_current)
        return true;

    wait_idle();

    if (g_current)
        g_current->clearProtocol();

    g_current = it->second;
    g_current->populateProtocol(&g_gui);
    return true;
}

unsigned int current()
{
    return g_current ? g_current->getID() : 0;
}

bool has(PIRKeyName key)
{
    return g_current && g_current->hasKey(key);
}

std::vector<RemoteInput> inputs()
{
    QComboBox cb;
    std::vector<RemoteInput> out;

    if (!g_current)
        return out;

    g_current->populateInputList(&cb);

    for (size_t i = 0; i < cb.items.size(); i++) {
        PIRKeyName key = (PIRKeyName)cb.items[i].data.toInt();

        if (g_current->hasKey(key))
            out.push_back(RemoteInput(cb.items[i].text.toStdString(), key));
    }
    return out;
}

bool press(PIRKeyName key)
{
    PIRProtocol *p;

    if (!g_current || !(p = Access::protocol(g_current)))
        return false;

    {
        QMutexLocker l(&commandIFMutex);
        if (commandInFlight)
            return false;
        commandInFlight = true;
    }

    {
        std::lock_guard<std::mutex> l(g_lock);
        g_job = p;
        g_job_id = g_current->getID();
        g_job_key = key;
        g_held = true;
    }
    g_cv.notify_all();
    return true;
}

void release()
{
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_held = false;
    }
    raise_stop();
    g_cv.notify_all();
}

bool busy()
{
    QMutexLocker l(&commandIFMutex);
    return commandInFlight;
}

bool take_error(std::string &out)
{
    std::lock_guard<std::mutex> l(g_lock);

    if (g_error.empty())
        return false;
    out.swap(g_error);
    g_error.clear();
    return true;
}

}
