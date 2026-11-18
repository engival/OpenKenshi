#pragma once

// The build's git hash ("<8 hex>" or "<8 hex>-dirty", "unknown" without git). githash.cpp is generated at build time by tools/githash.cmake,
// so a commit shows in --help without a reconfigure.
const char* kclient_git_hash();
