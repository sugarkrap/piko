#ifndef PIKO_REMOTE_H
#define PIKO_REMOTE_H

#include <string>
#include <utility>
#include <vector>

#include "pirkeynames.h"
#include "pirpanelnames.h"

struct RemoteKeyset {
    unsigned int id;
    std::string make;
    std::string name;
    PIRPanelTypes panels;
};

struct RemoteDevice {
    std::string make;
    std::string model;
    std::string type;
    unsigned int keyset;
};

typedef std::pair<std::string, PIRKeyName> RemoteInput;

namespace remote {

void load(void (*wake)(void));

const std::vector<RemoteKeyset> &keysets();
const RemoteKeyset *find(unsigned int id);
const RemoteKeyset *find(const std::string &make, const std::string &name);
std::vector<std::string> makes();
std::vector<RemoteDevice> devices();

bool select(unsigned int id);
unsigned int current();
bool has(PIRKeyName key);
std::vector<RemoteInput> inputs();

bool press(PIRKeyName key);
void release();
bool busy();

bool take_error(std::string &out);

}

#endif
