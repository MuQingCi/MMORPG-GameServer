#include "base/serviceApp.h"

int main(int argc, char** argv)
{
    return RunService(argc, argv, ServerID::kGlobal);
}