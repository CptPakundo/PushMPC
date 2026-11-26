# PushMPC in KMPlayer, GOM Player and Zoom Player

Setup sets PushMPC up by itself in MPC-HC, MPC-BE and PotPlayer. KMPlayer, GOM Player and Zoom Player keep their filter
lists in their own settings, so for them you add PushMPC once by hand. It takes a minute. Until you do, Setup gives
these players PushMPC's capture mode instead.

PushMPC Setup has already registered "PushMPC Filter" on your PC, so it shows up in each player's list of filters.
Close and reopen the player after the change. The next time PushMPC Setup runs, it sees the entry and turns capture
mode off for that player.

For HDR video, pick MPC Video Renderer or madVR as the player's video renderer if it offers them.

## KMPlayer 64X

KMPlayer 64X has the same settings window as MPC-HC.

1. Right-click the video and open **Options** (or press **O**).
2. Go to **External Filters** and click **Add Filter...**.
3. Pick **PushMPC Filter** from the list and click **OK**.
4. Select it in the list and choose **Prefer**.
5. Click **OK**.

## KMPlayer (32-bit)

1. Right-click the video and open **Preferences** (or press **F2**).
2. Go to **Filter Control** and open the list of filters it uses (the filter priority or external filter list).
3. Click **Add registered filter**, pick **PushMPC Filter**, and set it to **Prefer**.
4. Click **OK** or **Apply**.

## GOM Player

1. Right-click the video and open **Preferences**.
2. Open **Filter**, then the **Advanced Settings** tab.
3. Set **Filter Rendering Method** to **Advanced**.
4. Click **Add Filter**, pick **PushMPC Filter** and click **OK**.
5. Set its priority to **Highest** and click **OK**.

## Zoom Player

Zoom Player picks its decoders with Smart Play. Add PushMPC to the video decoders you use:

1. Open **Options** and go to **Playback > Smart Play**.
2. Open the video decoder settings for the formats you watch (for example H.264 and HEVC).
3. In each decoder chain, keep the decoder first and add **PushMPC Filter** after it.
4. Save the settings.

If you later run Smart Play's **Auto-Configure**, Zoom Player removes filters you added by hand. Add PushMPC again
afterwards. Setup checks this every time it runs and turns capture mode back on while PushMPC is missing.

## Removing it

Remove **PushMPC Filter** from the same list in the player. Uninstalling PushMPC unregisters the filter, and the player
then skips the entry.
