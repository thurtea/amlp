// mudlib:  library
// file:    /single/room_chamber_a.c
// purpose: one of three static test chambers reachable from the
//          entrance hall (/single/start_room.c) -- see that file's own
//          header comment for the full 2x2 layout this is part of.
//          Plain, undecorated rooms on purpose: this is a test mudlib
//          exercising real driver movement/exit machinery, not a themed
//          area, and the flat, honest naming says so.

#include <globals.h>

inherit ROOM_BASE;

void
create()
{
    set_exits((["south": START_LOC, "east": ROOM_CHAMBER_C]));
}

string
short()
{
    return "test chamber A";
}

string
long()
{
    return
        "A bare test chamber, identical in every way that matters to "
        "its two siblings (B and C) except its own position in the "
        "layout. Exits: " + exits_desc() + ".\n";
}

int
id(string arg)
{
    return arg == "room" || arg == "chamber a" || arg == "chamber" || arg == "test chamber a";
}

// No "look" command exists anywhere in this mudlib (see start_room.c's
// own header comment), so init() writes the room's own description to
// whoever just arrived, the same moment a "look" would have.
void
init()
{
    if (this_player()) {
        write(long());
    }
    room::init();
}
