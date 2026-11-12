# wheel

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

Header-only hierarchical timer wheel for C++: one owner thread arms, re-arms, cancels and fires
intrusive 32-byte entries with no allocation; other threads arm through a 64-stripe intrusive
inbox. The successor to [stash](https://github.com/Kronuz/stash) and the timer side of
[scheduler](https://github.com/Kronuz/scheduler); the evidence is in Detent's
`docs/design/wheel-evidence.md`.

Work in progress: the first release lands with the `wheel-lib` quest.
