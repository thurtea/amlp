// mudlib:  library
// file:    /single/start_room.c
// purpose: the entrance hall -- every fresh login lands here. One of
//          four static rooms this test mudlib now has (see ROOM_CHAMBER_*
//          in globals.h and each of their own files): a small 2x2 layout,
//          start_room <-> room_chamber_a (north/south) and start_room
//          <-> room_chamber_b (east/west), room_chamber_a <-> room_
//          chamber_c (east/west) and room_chamber_b <-> room_chamber_c
//          (north/south). Before this there was exactly one room and no
//          way to leave it at all.
//
// No "look" command exists anywhere in this mudlib (stock Lil never
// shipped one either -- see WAND_OF_CREATION_SCOPING.md's own note on
// this exact point), so init() below writes the room's own description
// straight to whoever just arrived, the same moment a "look" command
// would have, rather than leaving it unreachable.

#include <globals.h>

inherit ROOM_BASE;

void
create()
{
    set_exits((["north": ROOM_CHAMBER_A, "east": ROOM_CHAMBER_B]));
}

string
short()
{
    return "the entrance hall";
}

string
long()
{
    return
        "A plain stone entrance hall, the entry point of this test "
        "mudlib. A wand of creation rests on a low pedestal near the "
        "door. Exits: " + exits_desc() + ".\n"
        "Type 'help' for the full list of runnable commands.\n";
}

int
id(string arg)
{
    return arg == "room" || arg == "entrance hall" || arg == "entrance" || arg == "hall";
}

// init: fires every time a living moves into this room, not just the
// first time (a real MudOS/FluffOS apply, already recognized and fired
// by this driver -- see ApplyTable.cpp) -- so the wand-of-creation hand-
// out is guarded against handing out a second one to someone who
// already has one (walks out and back in, reconnects, etc.). The
// description write is unguarded -- showing it again on re-entry is
// exactly what a real "look" would do too.
//
// The handed-out wand is moved here in two steps, not one, matching
// wand_of_creation.c's own documented mechanism (see its own comment on
// held()/init()): this driver's VM::moveObject() only calls init() on
// the *moved* object (which is where the wand's own add_action calls
// live) when the destination's existing occupants already have
// commandsEnabled() true (true here because the player is already a
// genuine occupant of this room by the time this function runs, not
// true of the player's own (empty) inventory. Moving the wand straight
// into the player with a single move() call would leave its add_action
// calls never registered -- confirmed live the first time this was
// tried, all three wand commands silently fell through to commandHook()
// and failed with "source file not found".
//
// room::init() (a bare qualified parent call) registers this room's own
// exit verbs (north/east here) onto the arriving player, exactly the
// same "runs alongside, not instead of" pattern this file already used
// before room.c existed.
void
init()
{
    object player;
    object wand;

    player = this_player();
    if (!player) {
        return;
    }

    write(long());

    if (!present("wand", player)) {
        wand = clone_object(WAND_OB);
        if (wand) {
            wand->move(this_object()); // enters the room the player already occupies -- fires the wand's own init()
            wand->move(player);        // now genuinely held
        }
    }

    room::init();
}
