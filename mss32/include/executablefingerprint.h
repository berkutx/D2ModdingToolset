/*
 * Exact executable identity checks shared by production features and optional
 * test tooling. Keep raw-address features behind this module instead of
 * duplicating weaker size/version checks in individual subsystems.
 */

#ifndef EXECUTABLEFINGERPRINT_H
#define EXECUTABLEFINGERPRINT_H

namespace hooks {
namespace executablefingerprint {

/**
 * True only for the exact Russobit Discipl2.exe for which the raw hook addresses
 * are proven: 4,187,648 bytes, SHA-256
 * 1375CDEF09EC470EE64FE5693FB734D7C69FB215212311D997F792B258A642EB,
 * loaded at its fixed image base 0x00400000. Cached after the first check.
 */
bool isExactRussobit();

} // namespace executablefingerprint
} // namespace hooks

#endif // EXECUTABLEFINGERPRINT_H
