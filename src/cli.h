#pragma once

#include <QStringList>

class Cli {
public:
    static bool isCommand(const QStringList &arguments);
    static int run(const QStringList &arguments);
};
