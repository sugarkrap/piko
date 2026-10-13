#ifndef PIRSELECTDEVICEFORM_H
#define PIRSELECTDEVICEFORM_H

#include <vector>

#include "pirkeysetwidgetitem.h"

class PIRSelectDeviceForm {
public:
    ~PIRSelectDeviceForm()
    {
        for (size_t i = 0; i < items.size(); i++)
            delete items[i];
    }

    void addWidgetItem(PIRKeysetWidgetItem *kwi) { items.push_back(kwi); }

    std::vector<PIRKeysetWidgetItem *> items;
};

#endif
