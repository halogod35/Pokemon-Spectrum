#ifndef GUARD_MYSTERY_GIFT_DISTRIBUTOR_H
#define GUARD_MYSTERY_GIFT_DISTRIBUTOR_H

// PS-080: the built-in Mystery Gift distributor. It plays the part of the Nintendo distribution
// machine inside the game: when the player picks "Wireless Communication" on the Mystery Gift
// screen, the Mystery Gift server (mystery_gift_server.c) runs in-process on a distributor
// script and talks to the client (mystery_gift_client.c) through the loopback link
// (mystery_gift_link.c). No wireless adapter, no network, no threads: the server is ticked once
// per frame from the Mystery Gift task while the client runs.

// Starts a session: enables the loopback link and creates the server on the script for the next
// gift (Wonder Cards) or the "no news" message (Wonder News). The menu creates the client after
// this, so its link is a loopback link too.
void MysteryGiftDistributor_Start(bool32 isWonderNews);
// TRUE between Start and Stop/Finish.
bool32 MysteryGiftDistributor_IsRunning(void);
// Runs the server one step. Call once per frame while a session is running.
void MysteryGiftDistributor_Tick(void);
// TRUE once a block was refused on the loopback (bad size, ident or CRC): the menu must Stop
// and show the communication error.
bool32 MysteryGiftDistributor_Failed(void);
// Called every frame after the client ended. TRUE once the server has ended too (it still has
// the client's READY_END to read), or after a timeout; stops the session either way.
bool32 MysteryGiftDistributor_Finish(void);
// Aborts: destroys the client and the server if they are still alive and stops the loopback.
void MysteryGiftDistributor_Stop(void);

#endif //GUARD_MYSTERY_GIFT_DISTRIBUTOR_H
