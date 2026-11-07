#pragma once
#include <QStringList>

class Cli {
public:
    static int run(const QStringList &args);
    static void printHelp();
};
