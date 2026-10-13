#ifndef PIRKEYSETWIDGETITEM_H
#define PIRKEYSETWIDGETITEM_H

#include <QString>

#include "pirmakenames.h"
#include "pirdevicetypenames.h"

class PIRKeysetWidgetItem {
public:
    PIRKeysetWidgetItem(QString displayName, QString internalName,
                        unsigned int id, PIRMakeName make,
                        PIRDeviceTypeName type)
        : displayName(displayName), internalName(internalName), id(id),
          make(make), type(type)
    {
    }

    QString displayName;
    QString internalName;
    unsigned int id;
    PIRMakeName make;
    PIRDeviceTypeName type;
};

#endif
