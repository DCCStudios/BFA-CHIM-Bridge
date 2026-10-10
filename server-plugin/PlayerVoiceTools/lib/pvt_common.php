<?php
// Shared helpers for Player Voice Tools. Function names are prefixed pvt_.

if (!function_exists('pvt_engine_path')) {
    function pvt_engine_path(): string
    {
        return dirname(__DIR__, 3) . DIRECTORY_SEPARATOR;
    }

    function pvt_soundcache_path(): string
    {
        return pvt_engine_path() . 'soundcache' . DIRECTORY_SEPARATOR;
    }

    // Lowercase letters and digits only, so punctuation and spacing changes between the raw
    // request and the spoken line do not matter.
    function pvt_normalize(string $text): string
    {
        return preg_replace('/[^a-z0-9]+/', '', strtolower($text));
    }

    // The player's words without the "Name:" prefix and trailing "(Talking to X)" tags.
    function pvt_clean_line(string $raw): string
    {
        $line = trim($raw);
        $split = explode(':', $line, 2);
        if (count($split) === 2 && strlen($split[0]) <= 64) {
            $line = trim($split[1]);
        }
        $line = preg_replace('/\s*\([^()]*\)\s*$/', '', $line);
        return trim(preg_replace('/\s+/', ' ', str_replace(["\r", "\n", "|"], ' ', $line)));
    }

    function pvt_mood_path(string $normalized): string
    {
        return pvt_soundcache_path() . 'pvt_mood_' . md5($normalized) . '.txt';
    }

    // Starts classify.php in the background so the caller does not wait for the LLM.
    function pvt_spawn_classifier(string $line): void
    {
        $normalized = pvt_normalize($line);
        if ($normalized === '' || strlen($line) > 600) {
            return;
        }
        $moodPath = pvt_mood_path($normalized);
        $pendingPath = $moodPath . '.pending';
        if (is_file($moodPath) || (is_file($pendingPath) && time() - filemtime($pendingPath) < 10)) {
            return;
        }
        @touch($pendingPath);
        $script = escapeshellarg(dirname(__DIR__) . DIRECTORY_SEPARATOR . 'classify.php');
        @exec('nohup php ' . $script . ' ' . escapeshellarg(base64_encode($line)) . ' > /dev/null 2>&1 &');
    }

    // True while a classifier started in the last 10 seconds has not finished.
    function pvt_any_pending(): bool
    {
        foreach (glob(pvt_soundcache_path() . 'pvt_mood_*.pending') ?: [] as $marker) {
            if (time() - filemtime($marker) < 10) {
                return true;
            }
        }
        return false;
    }

    // "<type> <strength>" for a spoken line, or '' when nothing is known yet. Falls back to a
    // recent longer input that contains the line, since the server may split one input into
    // several spoken sentences.
    function pvt_lookup_mood(string $normalized): string
    {
        $moodPath = pvt_mood_path($normalized);
        if (is_file($moodPath)) {
            return trim(strtok(strval(@file_get_contents($moodPath)), "\n"));
        }
        if (strlen($normalized) < 6) {
            return '';
        }
        foreach (glob(pvt_soundcache_path() . 'pvt_mood_*.txt') ?: [] as $candidate) {
            if (time() - filemtime($candidate) > 300) {
                continue;
            }
            $parts = explode("\n", strval(@file_get_contents($candidate)), 2);
            if (count($parts) === 2 && strpos($parts[1], $normalized) !== false) {
                return trim($parts[0]);
            }
        }
        return '';
    }
}
