/* rawfish-ir-version.h -- the ONE version of the engine, shared by the Android
 * library (android/rawfish-ir.c) and the Sailfish CLI (sailfish/rawfish-ir.c). The CLI
 * refuses a library of another version (they are built and installed
 * separately, and a mismatch fails in confusing ways).  The rawfish-ir-cli RPM
 * (rpm/rawfish-ir-cli.spec) and the gui's carry the same number. */
#ifndef INDRAW_VERSION_H
#define INDRAW_VERSION_H
#define IR_VERSION "0.3.1"
#endif
