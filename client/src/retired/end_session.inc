// Retired (kenshi-k16.2.212.1): EXIT GAME now quits the program, so nothing ends a session back to the title. Not compiled; kept as the archive.
// It lived in main.cpp and used end_session_load() and start_local_server().

// EXIT GAME in single player (docs/CLIENT_GOALS.md "Session front end", slice 3): the session ends. The server is stopped, the sim view, its
// characters, ragdolls and blood dropped, the per-session client state cleared; the world-wide data (terrain, buildings, game data) stays loaded.
// The title then starts a fresh server, as at startup, so NEW / LOAD / CONTINUE (its newest save) work on it. A --simlink join keeps its server.
static void end_session()
{
	end_session_load();
	app.ragdolls.clear();	// before the characters: the ragdolls drive their bones
	app.sim.end();
	app.chars.clear();
	app.nav = ClientNav();
	app.blood.clear();
	app.server.stop();
	app.local = app.greeted = app.session = false;
	app.spectating = app.worldChosen = false;	// the next session's wire says who plays, once a world is chosen (kenshi-k16.2.95)
	app.seen = {};
	app.prospectSeen = app.weatherSeen = app.beamsSeen = 0;
	app.beams.clear();
	app.newGameStart.clear();
	app.newGameSent = false;
	app.newGameLeader = 0;
	app.menuPaused = app.menuUnpaused = false;
	app.quietPauseTo = -1;
	app.inspect = app.jobsChar = app.statsChar = app.infoChar = 0;
	app.panelProducer = app.focusLogged = app.rowLoggedId = app.gearChar = 0;	// the focus panel's subject went with the world (k16.19.8.2)
	app.gearQueues.clear();	// its inventories went with it (k16.20.14.2)
	app.focusLoggedProducer = false;
	app.rowLoggedVersion = 0;
	if (app.stats) { app.stats->show(false); }	// its character went with the world
	set_follow(false);
	app.centerId = 0;
	app.machine = 0;
	cont_close_all();
	app.lastSelect = 0;
	dev::leaveRun();
	app.mainMenu->show(true);
	audiohook_event("Boot_Up_Game");	// the title's music, as at startup
	start_local_server();
	printf("session: ended (EXIT GAME); a fresh server is starting\n");
}
