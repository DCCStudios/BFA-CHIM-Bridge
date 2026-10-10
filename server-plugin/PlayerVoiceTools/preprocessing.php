<?php
// Player Voice Tools: when the player says something, start labelling its emotion in the
// background. The label is ready by the time the line's audio has been generated.

require_once(__DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'pvt_common.php');

$pvtPlayerSpeechRequests = [
    'inputtext', 'inputtext_s', 'ginputtext', 'ginputtext_s', 'narrator_inputtext',
    'player_menu_tts_prefetch', 'player_menu_tts_play',
];

if (isset($gameRequest[0], $gameRequest[3]) && in_array($gameRequest[0], $pvtPlayerSpeechRequests, true)) {
    $pvtLine = pvt_clean_line(strval($gameRequest[3]));
    if ($pvtLine !== '') {
        pvt_spawn_classifier($pvtLine);
    }
}
