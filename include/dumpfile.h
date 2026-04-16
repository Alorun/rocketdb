#pragma once

#include <string>

#include "env.h"
#include "export.h"
#include "status.h"

namespace rocketdb {

Status DumpFile(Env* env, const std::string& fname, WritableFile* dst);

}