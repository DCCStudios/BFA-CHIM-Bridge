<?php
// Player Voice Tools: GET mood.php?text=<spoken line> returns "<type> <strength>".
// Asked by the CHIM BFA Bridge game plugin when a player line's audio has downloaded.
// Answers "0 0" (neutral) when no label is ready, and never waits longer than 350 ms.

require_once(__DIR__ . DIRECTORY_SEPARATOR . 'lib' . DIRECTORY_SEPARATOR . 'pvt_common.php');

header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store');

$text = strval($_GET['text'] ?? '');
$normalized = pvt_normalize($text);
if ($normalized === '' || strlen($text) > 600) {
    echo '0 0';
    return;
}

$deadline = microtime(true) + 0.35;
do {
    clearstatcache();
    $mood = pvt_lookup_mood($normalized);
    if ($mood !== '' || !pvt_any_pending()) {
        break;
    }
    usleep(25000);
} while (microtime(true) < $deadline);

if ($mood === '') {
    // Nothing known: label this exact line now so a repeat of it has one.
    pvt_spawn_classifier(trim($text));
    $mood = '0 0';
}
echo preg_match('/^\d \d{1,3}$/', $mood) ? $mood : '0 0';
