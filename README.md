# BFA-CHIM-Bridge

Sends CHIM player speech and its emotion to Better Facial Animations (BFA), so the player's face is animated from the real TTS audio.

CHIM plays TTS through its own audio player, so the game engine never speaks a voice file and BFA has nothing to animate. This project fills that gap for the player character. NPC lines are not covered.

It has two parts:

| Part | Folder | Runs in |
|---|---|---|
| SKSE plugin `CHIMBFABridge.dll` | [skse-plugin/](skse-plugin/) | the game |
| CHIM server extension `PlayerVoiceTools` | [server-plugin/PlayerVoiceTools/](server-plugin/PlayerVoiceTools/) | HerikaServer |

The SKSE plugin alone gives lip and face animation with a neutral mood. The server extension adds an emotion per line.

## Tested with

- Skyrim SE 1.6.1170, SKSE 2.2.6
- CHIM 3.4.2 (AIAgent.dll) and HerikaServer 3.4.2
- Better Facial Animations 3.2.1

Other versions may work. Both parts depend on internals of CHIM and BFA, listed under [Limits](#limits).

## Install

**SKSE plugin.** Build it (see below) and put `CHIMBFABridge.dll` in `Data/SKSE/Plugins/`. Do not install it together with Dragonborn ReVoiced. The plugin registers with SKSE under the name `DBReV`, because that is the sender BFA listens to for player speech.

**Server extension.** Copy `server-plugin/PlayerVoiceTools/` to `HerikaServer/ext/PlayerVoiceTools/`. It changes no core server files. To choose the LLM that labels emotions, add a row to the server's `conf_opts` table:

```sql
INSERT INTO conf_opts (id, value) VALUES ('player_emotion_connector_id', '<id from core_llm_connector>');
```

Without that row it uses the first connector with a chat-completions URL. A small, fast model is enough.

In BFA, keep **Player facial animations** on, and use a third-person camera to see the result.

## How it works

1. The CHIM server sends each spoken line to the game as `Speaker|ScriptQueue|text/...`. The plugin watches that stream through AIAgent.dll's import of `recv` and notes lines whose speaker is `Player`.
2. AIAgent.dll then downloads `/soundcache/<md5 of text>.wav` with WinHTTP. The plugin copies that download to `Data/Sound/CHIMBFABridge/<hash>.wav`.
3. The plugin asks the server extension for the line's emotion (`ext/PlayerVoiceTools/mood.php?text=...`), which answers `<type> <strength>` using Skyrim's dialogue emotion types 0 to 7.
4. BFA's player speech path passes a constant 0 for emotion type and value. The plugin finds those two constants in `BetterFacialAnimations.dll` at startup and writes the line's emotion over them.
5. The plugin sends BFA the Dragonborn ReVoiced `kPlayerLineStart` message with the file path and audio length. BFA generates and plays the animation.

On the server, `preprocessing.php` starts the emotion label in the background as soon as the player's text arrives, so it runs while the TTS audio is generated and does not delay the line.

The server extension also lets Cartesia and Inworld connectors use voices created on the provider's site (voice IDs and voice names) in place of local samples. See its [README](server-plugin/PlayerVoiceTools/README.md).

## Logs

`Documents/My Games/Skyrim Special Edition/SKSE/CHIMBFABridge.log`. A working line looks like this:

```text
player line 'This is obviously a shakedown.' -> a6a19af1cf619b1724aaf697c3db568a
player speech CHIMBFABridge\a6a19af1cf619b1724aaf697c3db568a.wav (2.15 s, emotion 2/75) sent to listeners
```

`BetterFacialAnimations.log` then shows `player speech:`, `gen ... emo 2/75 ... fed disgust`, `play` and `end` for the same file.

## Limits

- **BFA updates.** The emotion patch looks for one specific instruction sequence in BFA's DLL. If it is not found exactly once, the plugin logs that emotions are off and animation continues with a neutral mood.
- **CHIM updates.** The plugin relies on CHIM's line format and on its audio being fetched from `/soundcache/` over WinHTTP. If either changes, the plugin stops seeing player lines and logs nothing after startup.
- **Late labels.** The plugin waits up to 500 ms for an emotion. A line whose label is not ready plays neutral.
- **Cache files.** WAV copies in `Data/Sound/CHIMBFABridge/` are removed at each game start. Under Mod Organizer 2 they land in Overwrite.
- **CHIM's own lips.** CHIM still runs its lip animation on the player. If the two fight, set CHIM's lip animation intensity to 0.

## Build the SKSE plugin

Needs Visual Studio 2022, CMake 3.21+ and vcpkg with `VCPKG_ROOT` set. CommonLibSSE NG is built from source:

```powershell
cd skse-plugin
git clone -b ng --recurse-submodules https://github.com/alandtse/CommonLibVR.git extern/CommonLibVR
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The DLL is written to `skse-plugin/Compile/SKSE/Plugins/`. To use an existing CommonLib checkout, pass `-DCOMMONLIBSSE_DIR=<path>` to the first `cmake` command.

## Credits

- [Better Facial Animations](https://gitlab.com/cyberquach/better-facial-animations) by Cyberquach. This project contains none of its code. It talks to BFA through the public Dragonborn ReVoiced message contract and patches two constants in memory.
- [CHIM](https://www.nexusmods.com/skyrimspecialedition/mods/126330) by Dwemer Dynamics.
- Dragonborn ReVoiced by Raynor1511, for the message contract BFA accepts.
