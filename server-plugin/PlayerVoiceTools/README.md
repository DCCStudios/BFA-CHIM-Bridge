# Player Voice Tools

A HerikaServer extension. Install by copying this folder to `HerikaServer/ext/PlayerVoiceTools/`. It changes no core files, so a server update leaves it in place.

Tested on HerikaServer 3.4.2 (branch `aiagent`, commit `fc8778f`).

## Provider voice IDs (`globals.php`)

Stock Cartesia and Inworld connectors treat a voice setting as the name of a sample in `data/voices/` and try to clone it. This hook writes the connector's own cache rows in `conf_opts` ahead of time, so these settings work without a sample:

| Voice setting | Result |
|---|---|
| Cartesia voice ID (a UUID) | used as is |
| Inworld voice ID (`workspace__voice`) | used as is, for every Inworld connector |
| Any other name | looked up once in the Cartesia library and the Inworld voice list |

It checks the player's voice override, every NPC voice ID, and the voice typed into the TTS Connector test. Saved settings are rescanned once a minute. A name that no provider knows is retried after a day.

Rows it writes: `cartesia_voice_id_<voice>`, `inworld_voice_scope_<hash>__<voice>`, and `pvt_voice_miss_*` for failed lookups. The "forget cached voice ID" button on the voice cloning page removes the first two.

## Player emotion (`preprocessing.php`, `classify.php`, `mood.php`)

When the player speaks (chat, voice input or a dialogue-menu topic), `preprocessing.php` starts `classify.php` in the background. It asks an LLM for the emotion of the line and saves it in `soundcache/pvt_mood_<hash>.txt`. The TTS call runs at the same time, so the line is not delayed.

`mood.php?text=<spoken line>` returns `<type> <strength>`: type 0 to 7 (neutral, anger, disgust, fear, sad, happy, surprise, puzzled) and strength 0 to 100. It waits at most 350 ms and answers `0 0` when no label is ready.

The LLM connector is the row `player_emotion_connector_id` in `conf_opts` (an ID from `core_llm_connector`). Without that row it uses the first connector with a chat-completions URL.

## Limits

- The voice ID hook depends on the cache key format of the stock connectors. If a server update changes those keys, voice IDs stop resolving and the connectors fall back to cloning.
- `mood.php` is reachable by anything that can reach the server and can trigger one LLM call per new line of up to 600 characters.
