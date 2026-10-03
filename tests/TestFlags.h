#pragma once

// Set by main() from the command line. --quiet skips the tests that make sound
// (ISSUES.md: only run audible tests when audio-related code changed).
inline bool skipAudibleTests = false;
