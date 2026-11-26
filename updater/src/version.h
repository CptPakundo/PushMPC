// PushMpcUpdater version (follows the PushMPC release it ships with)
#pragma once
#define PUSHMPC_UPDATER_VERSION_STR "1.0.0"
#define PUSHMPC_UPDATER_VERSION_W L"1.0.0"
#define PUSHMPC_UPDATER_VERSION_COMMA 1,0,0,0
// 1 = developer build (unsigned feeds allowed by default); the release build sets PUSHMPC_UPDATER_DEV=0
#ifndef PUSHMPC_UPDATER_DEV
#define PUSHMPC_UPDATER_DEV 1
#endif
