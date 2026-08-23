// mudlib:  library
// file:    /single/room_chamber_b.c
// purpose: one of three static test chambers reachable from the
//          entrance hall (/single/start_room.c) -- see that file's own
//          header comment for the full 2x2 layout this is part of.

#include <globals.h>

inherit ROOM_BASE;

void
create()
{
    set_exits((["west": START_LOC, "north": ROOM_CHAMBER_C]));
}

string
short()
{
    return "test chamber B";
}

string
long()
{
    return
        "A bare test chamber, identical in every way that matters to "
        "its two siblings (A and C) except its own position in the "
        "layout. Exits: " + exits_desc() + ".\n";
}

int
id(string arg)
{
    return arg == "room" || arg == "chamber b" || arg == "chamber" || arg == "test chamber b";
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
