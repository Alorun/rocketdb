#include "../../include/options.h"

#include "../../include/comparator.h"
#include "../../include/env.h"

namespace rocketdb {

Options::Options() : comparator(BytewiseComparator()), env(Env::Default()) {}

} 
