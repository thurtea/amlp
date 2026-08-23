// mudlib:  library
// file:    /single/room_chamber_c.c
// purpose: one of three static test chambers reachable from the
//          entrance hall (/single/start_room.c) -- see that file's own
//          header comment for the full 2x2 layout this is part of.
//          Reachable two ways (via chamber A going east, or via chamber
//          B going north), closing the loop.

#include <globals.h>

inherit ROOM_BASE;

void
create()
{
    set_exits((["west": ROOM_CHAMBER_A, "south": ROOM_CHAMBER_B]));
}

string
short()
{
    return "test chamber C";
}

string
long()
{
    return
        "A bare test chamber, identical in every way that matters to "
        "its two siblings (A and B) except its own position in the "
        "layout -- this one closes the loop, reachable from either "
        "direction. Exits: " + exits_desc() + ".\n";
}

int
id(string arg)
{
    return arg == "room" || arg == "chamber c" || arg == "chamber" || arg == "test chamber c";
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
