#pragma once
#include <optional>
#include <string>

namespace amlp {

// The FluffOS uid / euid object trust model. ROADMAP row 3.1.
//
// Real source: temp/reference/fluffos-2.9-ds2.08/object.h:107-108
// (object_t carries "userid_t *uid" the owner and "userid_t *euid" the
// effective owner), packages/uids.c (the userid_t struct, add_uid(),
// f_getuid/f_geteuid/f_seteuid/f_export_uid), packages/uids_spec.c (the
// four efun signatures), master.c:107-138 (set_master() sets
// master_ob->uid = set_root_uid(get_root_uid()) and master_ob->euid =
// master_ob->uid, then set_backbone_uid(get_bb_uid())), and simulate.c:
// 132-206 (give_uid_to_object(), the per-load/per-clone assignment).
//
// This driver has no PACKAGE_UIDS compile flag, so the model is "active"
// exactly when the loaded master defined get_root_uid() and it returned
// a string at boot (see captureBootUids() in ObjectManager). When it is
// not active the four efuns still work on whatever per-object euid a
// mudlib sets by hand, they just never see an owner uid assigned for
// them, matching a mudlib built without PACKAGE_UIDS.
//
// Interned-string sharing (real add_uid()'s AVL tree of shared strings)
// is not reproduced: every efun that observes a uid reads only its
// .name, so a plain owned std::string is observationally identical.
struct UidModel {
    // master.c: set_root_uid(get_root_uid()). The mud's trust root.
    std::optional<std::string> rootUid;
    // master.c: set_backbone_uid(get_bb_uid()). The shared library uid.
    std::optional<std::string> backboneUid;

    bool active() const { return rootUid.has_value(); }
};

// The (uid, euid) a freshly loaded or cloned object should receive,
// distilled from real simulate.c give_uid_to_object() (2.9 ds2.08).
struct ResolvedObjectUids {
    std::string uid;
    // std::nullopt is real's "euid = NULL" (AUTO_SETEUID is #undef in
    // the vendored local_options, so a fresh non-loader-owned object
    // gets euid 0 and must seteuid() itself, the "void create() {
    // seteuid(getuid()); }" corpus idiom).
    std::optional<std::string> euid;
};

// creatorName: what the master's creator_file(path) returned for the new
// object. loaderUid / loaderEuid: current_object->uid / ->euid at the
// load site (both taken as the root uid when there is no current object,
// e.g. a boot preload).
//
// Real branches reproduced:
//   - creatorName == loaderUid    -> uid = loaderUid, euid = loaderEuid
//     ("the loaded object has the same uid as the loader").
//   - otherwise                   -> uid = creatorName, euid = NULL
//     ("defined by another wizard ... can't be trusted").
// Real's AUTO_TRUST_BACKBONE branch (uid = euid = loaderEuid when
// creatorName == backbone) is NOT taken: AUTO_TRUST_BACKBONE is #undef
// in the vendored build. A build that defined it would add that check
// between the two branches above.
ResolvedObjectUids resolveObjectUids(const UidModel& model,
                                     const std::string& creatorName,
                                     const std::optional<std::string>& loaderUid,
                                     const std::optional<std::string>& loaderEuid);

} // namespace amlp
