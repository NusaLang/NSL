#include "i18n.hpp"

namespace i18n {

Lang& current() {
    static Lang* lang = new Lang(Lang::ID);
    return *lang;
}

}  // namespace i18n
