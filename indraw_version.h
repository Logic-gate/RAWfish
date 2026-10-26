/* indraw_version.h -- the ONE version of the engine, shared by the Android
 * library (android/indraw.c) and the Sailfish CLI (sailfish/indraw.c). The CLI
 * refuses a library of another version (they are built and installed
 * separately, and a mismatch fails in confusing ways). The indraw-engine RPM
 * (rpm/indraw-engine.spec) carries the same number. */
#ifndef INDRAW_VERSION_H
#define INDRAW_VERSION_H
#define IR_VERSION "0.47.70"
#endif
