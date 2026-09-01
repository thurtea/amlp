#include "amlp/security/UidModel.hpp"

namespace amlp {

ResolvedObjectUids resolveObjectUids(const UidModel& model,
                                     const std::string& creatorName,
                                     const std::optional<std::string>& loaderUid,
                                     const std::optional<std::string>& loaderEuid) {
    (void)model;  // backboneUid is only consulted by the AUTO_TRUST_BACKBONE
                  // branch, which the vendored build does not compile in.

    // real simulate.c:166 "if (strcmp(current_object->uid->name,
    // creator_name) == 0)": the new object is created from a path its
    // loader owns, so it inherits the loader's identity outright.
    if (loaderUid && *loaderUid == creatorName) {
        // real :171/:175 (non-COMPAT_32): ob->uid = current_object->uid;
        // ob->euid = current_object->euid;
        return {*loaderUid, loaderEuid};
    }

    // real :200-204: ob->uid = add_uid(creator_name); euid = NULL
    // (AUTO_SETEUID #undef). The object is "defined by another wizard",
    // owned by that wizard but with no effective uid until it calls
    // seteuid() itself.
    return {creatorName, std::nullopt};
}

} // namespace amlp
