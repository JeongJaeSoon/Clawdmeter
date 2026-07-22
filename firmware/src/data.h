#pragma once
#include <Arduino.h>

enum UsageProvider : uint8_t {
    USAGE_PROVIDER_CLAUDE = 0,
    USAGE_PROVIDER_CODEX = 1,
    USAGE_PROVIDER_COUNT = 2,
};

struct ProviderUsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[24];         // "allowed", "limited", or provider-specific error
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until this provider has data
};

struct UsageData {
    ProviderUsageData providers[USAGE_PROVIDER_COUNT];
    UsageProvider primary_provider;  // legacy top-level fields map here
    bool dual;                       // true when both providers were received
    bool valid;                      // false until first successful parse
};
