#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>
#include <omp.h>
#include <stdint.h>

#define MAX_LINE_LEN 256
#define INITIAL_CAPACITY 8192
#define NUM_SIMS 1000000
#define NUM_STEPS 240 // 10 days of hourly steps
#define DT (1.0 / 6240.0) // Annualized step for 1 hour

// -------------------------------------------------------------------------
// 1. DATA STRUCTURES & LIBCURL CALLBACKS
// -------------------------------------------------------------------------
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

typedef struct {
    double *prices;
    double *volumes;
    size_t count;
    size_t capacity;
} MarketData;

void init_market_data(MarketData *md) {
    md->capacity = INITIAL_CAPACITY;
    md->count = 0;
    md->prices = (double *)malloc(md->capacity * sizeof(double));
    md->volumes = (double *)malloc(md->capacity * sizeof(double));
}

void append_data(MarketData *md, double price, double volume) {
    if (md->count >= md->capacity) {
        md->capacity *= 2;
        md->prices = (double *)realloc(md->prices, md->capacity * sizeof(double));
        md->volumes = (double *)realloc(md->volumes, md->capacity * sizeof(double));
    }
    md->prices[md->count] = price;
    md->volumes[md->count] = volume;
    md->count++;
}

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

// -------------------------------------------------------------------------
// 2. DATA FETCHERS (FRED, TWELVE DATA, FOREX FACTORY, WORLD BANK)
// -------------------------------------------------------------------------

// A. FRED Mapping for Regional Series IDs or Assets
const char* get_fred_series_id(const char *symbol) {
    if (strstr(symbol, "XAU") != NULL) return "DFII10"; // Gold driver (10-Year TIPS Real Yield)
    if (strstr(symbol, "XAG") != NULL) return "DFII10"; // Silver driver
    if (strcmp(symbol, "USD") == 0) return "DGS10";     // US 10-Year Yield
    if (strcmp(symbol, "EUR") == 0) return "IR3TIB01EZM156N"; // Euro Area Interbank Rate
    if (strcmp(symbol, "GBP") == 0) return "IR3TIB01GBM156N"; // UK Interbank Rate
    if (strcmp(symbol, "JPY") == 0) return "IR3TIB01JPM156N"; // Japan Interbank Rate Proxy
    return "DGS10"; // Default fallback
}

// B. FRED: Dynamic Macro Driver Fetcher
double fetch_fred_yield(const char *fred_api_key, const char *series_id) {
    CURL *curl;
    MemoryStruct chunk = { malloc(1), 0 };
    double yield = 4.0; // Safe default fallback
    char url[512];
    
    snprintf(url, sizeof(url), 
        "https://api.stlouisfed.org/fred/series/observations?series_id=%s&api_key=%s&file_type=json&sort_order=desc&limit=1", 
        series_id, fred_api_key);

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        
        if (curl_easy_perform(curl) == CURLE_OK) {
            char *val_ptr = strstr(chunk.memory, "\"value\":\"");
            if (val_ptr) {
                val_ptr += 9;
                if (val_ptr[0] != '.') yield = atof(val_ptr);
            }
        }
        curl_easy_cleanup(curl);
    }
    free(chunk.memory);
    return yield / 100.0; 
}

// C. World Bank Open Data API Fetcher (No API Key Required)
double fetch_world_bank_indicator(const char *country_code, const char *indicator_id) {
    CURL *curl;
    MemoryStruct chunk = { malloc(1), 0 };
    double indicator_val = 0.0;
    char url[512];
    
    snprintf(url, sizeof(url), 
        "https://api.worldbank.org/v2/country/%s/indicator/%s?format=json&per_page=1", 
        country_code, indicator_id);

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        
        if (curl_easy_perform(curl) == CURLE_OK) {
            char *val_ptr = strstr(chunk.memory, "\"value\":");
            if (val_ptr) {
                val_ptr += 8;
                if (*val_ptr != 'n' && *val_ptr != 'N') {
                    indicator_val = atof(val_ptr);
                }
            }
        }
        curl_easy_cleanup(curl);
    }
    free(chunk.memory);
    return indicator_val;
}

// D. Twelve Data: Market Prices (Supports Currencies and Commodities like XAU/USD)
int fetch_twelvedata_csv(const char *api_key, const char *symbol, MemoryStruct *chunk) {
    CURL *curl = curl_easy_init();
    char url[512];
    snprintf(url, sizeof(url), 
        "https://api.twelvedata.com/time_series?symbol=%s&interval=1h&outputsize=2000&format=csv&order=asc&apikey=%s",
        symbol, api_key);

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) return -1;
    }
    return 0;
}

// E. Forex Factory: Economic Calendar XML
int fetch_forex_factory_xml(MemoryStruct *chunk) {
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://nfs.faireconomy.media/ff_calendar_thisweek.xml"); 
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res == CURLE_OK) return 0;
    }
    return -1;
}

// -------------------------------------------------------------------------
// 3. C-STRING PARSERS 
// -------------------------------------------------------------------------
void parse_memory_csv(const char *csv_data, MarketData *md) {
    if (csv_data[0] == '{') return; 

    char *data_copy = strdup(csv_data);
    char *line = strtok(data_copy, "\n");
    line = strtok(NULL, "\n"); 
    
    while (line != NULL) {
        char *token, *rest = line;
        int col = 0;
        double close_val = 0.0, vol_val = 0.0;
        
        while ((token = strtok_r(rest, ";,", &rest))) {
            if (col == 4) close_val = atof(token);
            if (col == 5) vol_val = atof(token);
            col++;
        }
        if (close_val > 0.0) append_data(md, close_val, vol_val > 0 ? vol_val : 1.0); 
        line = strtok(NULL, "\n");
    }
    free(data_copy);
}

int get_high_impact_event_count(const char *xml, const char *target_currency) {
    int count = 0;
    char search_country[32];
    snprintf(search_country, sizeof(search_country), "<country>%s</country>", target_currency);
    
    const char *ptr = xml;
    while ((ptr = strstr(ptr, "<event>")) != NULL) {
        const char *end_event = strstr(ptr, "</event>");
        if (!end_event) break;
        
        char *country_ptr = strstr((char *)ptr, search_country);
        char *impact_ptr = strstr((char *)ptr, "<impact>High</impact>");
        
        if (country_ptr && country_ptr < end_event && impact_ptr && impact_ptr < end_event) {
            count++;
        }
        ptr = end_event;
    }
    return count;
}

// -------------------------------------------------------------------------
// 4. PREDICTIVE CALIBRATION (Momentum & Volatility)
// -------------------------------------------------------------------------
void calibrate_predictive_model(const MarketData *md, double *sigma_out, double *momentum_drift) {
    if (md->count < 120) return;
    size_t num_returns = md->count - 1;
    double sum_sq_diff = 0.0, sum_returns = 0.0, avg_vol = 0.0, recent_vol = 0.0;

    for (size_t i = 0; i < num_returns; i++) {
        sum_returns += log(md->prices[i + 1] / md->prices[i]);
        avg_vol += md->volumes[i];
    }
    
    double mean_return = sum_returns / num_returns;
    for (size_t i = 0; i < num_returns; i++) {
        double diff = log(md->prices[i + 1] / md->prices[i]) - mean_return;
        sum_sq_diff += diff * diff;
    }
    *sigma_out = sqrt(sum_sq_diff / (num_returns - 1)) * sqrt(6240.0);

    for (size_t i = md->count - 25; i < md->count; i++) recent_vol += md->volumes[i];
    
    double volume_multiplier = (recent_vol / 24.0) / (avg_vol / num_returns); 
    double short_term_return = log(md->prices[md->count - 1] / md->prices[md->count - 25]);
    *momentum_drift = (short_term_return * (6240.0 / 24.0)) * volume_multiplier;
}

// -------------------------------------------------------------------------
// 5. HIGH-PERFORMANCE PRNG
// -------------------------------------------------------------------------
static inline uint64_t rotl(const uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
uint64_t next_xoshiro(uint64_t s[4]) {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}
double next_double(uint64_t s[4]) { return (next_xoshiro(s) >> 11) * 0x1.0p-53; }
double next_normal(uint64_t s[4]) {
    double u1 = next_double(s) + 1e-15; 
    double u2 = next_double(s);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}
int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

// -------------------------------------------------------------------------
// 6. MAIN PREDICTIVE PIPELINE
// -------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <SYMBOL/PAIR>\nExample: %s EUR/GBP or %s XAU/USD\n", argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    const char *target = argv[1];

    // !!! PASTE YOUR TWELVE DATA & FRED API KEYS HERE !!!
    const char *TWELVE_DATA_API_KEY = "YOUR TWELVE_DATA_API_KEY HERE"; 
    const char *FRED_API_KEY        = "YOUR FRED API HERE";
    
    curl_global_init(CURL_GLOBAL_ALL);

    double gravity_drift = 0.0;
    int is_commodity = (strstr(target, "XAU") != NULL || strstr(target, "XAG") != NULL);

    if (is_commodity) {
        // --- Commodity Macro Gravity (Real Yield Inverse Drag + World Bank Structural Adjustment) ---
        printf("Fetching Real Yield Macro Driver for Commodity from FRED...\n");
        double real_yield = fetch_fred_yield(FRED_API_KEY, get_fred_series_id(target));
        
        // Optional World Bank structural enhancement for global indicators (e.g., global inflation proxy)
        double global_inflation = fetch_world_bank_indicator("WLD", "FP.CPI.TOTL.ZG") / 100.0;
        
        gravity_drift = -real_yield + (global_inflation * 0.1);
    } else {
        // --- Currency Pair Macro Gravity (Interest Rate Differential) ---
        char base_curr[4] = {0}, quote_curr[4] = {0};
        strncpy(base_curr, target, 3);
        strncpy(quote_curr, target + 4, 3);

        printf("Fetching Regional Macroeconomic Gravity from FRED...\n");
        double base_rate = fetch_fred_yield(FRED_API_KEY, get_fred_series_id(base_curr));
        double quote_rate = fetch_fred_yield(FRED_API_KEY, get_fred_series_id(quote_curr));
        gravity_drift = base_rate - quote_rate; 
    }

    // --- Fetch Economic Calendar (Forex Factory) ---
    printf("Fetching Economic Calendar from Forex Factory...\n");
    MemoryStruct cal_mem = { malloc(1), 0 };
    fetch_forex_factory_xml(&cal_mem);
    
    int high_impact_events = get_high_impact_event_count(cal_mem.memory, is_commodity ? "USD" : target);

    // --- Fetch Market Data (Twelve Data) ---
    printf("Fetching Live Market Data for %s...\n", target);
    MemoryStruct mem = { malloc(1), 0 };
    fetch_twelvedata_csv(TWELVE_DATA_API_KEY, target, &mem);
    
    MarketData md; 
    init_market_data(&md);
    parse_memory_csv(mem.memory, &md);
    
    if (md.count < 120) {
        fprintf(stderr, "\n[ERROR] Failed to load market data for %s.\n", target);
        return EXIT_FAILURE;
    }
    
    double S0 = md.prices[md.count - 1];
    double sigma, momentum_drift;
    calibrate_predictive_model(&md, &sigma, &momentum_drift);

    printf("\n--- Predictive Calibration (%s) ---\n", is_commodity ? "Commodity" : "Forex");
    printf("Current Spot (S0):   %.5f\n", S0);
    printf("Macro Gravity Drift: %.2f%%\n", gravity_drift * 100.0);
    printf("Momentum Drift:      %.2f%%\n", momentum_drift * 100.0);
    printf("High-Impact Events:  %d Scheduled This Week\n", high_impact_events);

    const double jump_intensity = is_commodity ? (14.0 + (high_impact_events * 6.0)) : (12.0 + (high_impact_events * 5.0)); 
    const double jump_mean = 0.0;
    const double jump_vol = is_commodity ? (sigma * (1.8 + (high_impact_events * 0.2))) : (sigma * (1.5 + (high_impact_events * 0.15))); 

    double *final_prices = malloc(NUM_SIMS * sizeof(double));
    
    printf("\nRunning 1,000,000 Predictive Paths...\n");
    double start_time = omp_get_wtime();
    
    // --- OpenMP Simulation Engine ---
    #pragma omp parallel 
    {
        int tid = omp_get_thread_num();
        uint64_t s[4] = { 
            0x1234567890ABCDEFULL ^ (tid * 0x9E3779B97F4A7C15ULL), 
            0xFEDCBA0987654321ULL ^ (tid * 0xBF58476D1CE4E5B9ULL), 
            0x13579BDF2468ACE0ULL, 0x0ECA8642FDB97531ULL 
        };
        for(int i=0; i<10; i++) next_xoshiro(s);

        #pragma omp for schedule(static)
        for (int i = 0; i < NUM_SIMS; i++) {
            double S = S0;

            for (int t = 0; t < NUM_STEPS; t++) {
                double dynamic_drift = momentum_drift + gravity_drift;
                double v = (next_double(s) < 0.05) ? sigma * (is_commodity ? 1.6 : 1.5) : sigma * 0.85;
                double Z = next_normal(s);
                double jump_m = 1.0;

                if (next_double(s) < jump_intensity * DT) {
                    jump_m = exp(jump_mean + jump_vol * next_normal(s));
                }

                S *= exp((dynamic_drift - 0.5 * v * v) * DT + v * sqrt(DT) * Z) * jump_m;
            }
            final_prices[i] = S;
        }
    }
    double end_time = omp_get_wtime();

    // --- Decision Engine, Kelly Criterion & Tail Risk ---
    qsort(final_prices, NUM_SIMS, sizeof(double), compare_doubles);
    
    double target_price = final_prices[(int)(NUM_SIMS * 0.50)];
    double expected_profit_pct = ((target_price / S0) - 1.0) * 100.0;
    
    double worst_case_price = final_prices[(int)(NUM_SIMS * 0.01)];
    double max_loss_pct = ((worst_case_price / S0) - 1.0) * 100.0;
    
    double risk_reward_ratio = (fabs(max_loss_pct) > 0.001) ? fabs(expected_profit_pct / max_loss_pct) : 0.0;

    long wins = 0;
    double total_win_pct = 0.0;
    double total_loss_pct = 0.0;

    for (int i = 0; i < NUM_SIMS; i++) {
        double ret = (final_prices[i] / S0) - 1.0;
        if (ret > 0) {
            wins++;
            total_win_pct += ret;
        } else {
            total_loss_pct += fabs(ret);
        }
    }

    double p = (double)wins / NUM_SIMS;
    double q = 1.0 - p;
    double avg_win = (wins > 0) ? (total_win_pct / wins) : 0.001;
    double avg_loss = ((NUM_SIMS - wins) > 0) ? (total_loss_pct / (NUM_SIMS - wins)) : 0.001;
    double b = (avg_loss > 0) ? (avg_win / avg_loss) : 1.0;
    
    double kelly_fraction = (b > 0) ? ((p * b - q) / b) : 0.0;
    double half_kelly_pct = kelly_fraction * 50.0; 
    if (half_kelly_pct < 0.0) half_kelly_pct = 0.0;
    if (half_kelly_pct > 25.0) half_kelly_pct = 25.0;

    printf("\n=== UNIFIED ASYMMETRIC DECISION OUTPUT ===\n");
    printf("Compute Time: %.3f seconds\n\n", end_time - start_time);
    
    printf("[10-Day Predictive Target]\n");
    printf("-> Median Price:     %.5f\n", target_price);
    printf("-> Expected %s:    %+.2f%%\n", (expected_profit_pct >= 0) ? "Gain" : "Drop", expected_profit_pct);

    printf("\n[10-Day Safety Net]\n");
    printf("-> Worst-Case Price: %.5f\n", worst_case_price);
    printf("-> 99%% Max Loss:     %.2f%%\n", max_loss_pct);

    printf("\n[Execution Verdict]\n");
    printf("Risk-to-Reward Ratio: 1 : %.2f\n", risk_reward_ratio);
    
    if (risk_reward_ratio > 1.5 && expected_profit_pct > 0) {
        printf("Verdict: STRONG BUY (Reward heavily outweighs tail risk)\n");
        printf("Recommended Position Size (Half-Kelly): Risk %.2f%% of total account equity\n", half_kelly_pct);
    } else if (risk_reward_ratio > 1.5 && expected_profit_pct < 0) {
        printf("Verdict: STRONG SELL (Reward heavily outweighs tail risk)\n");
        printf("Recommended Position Size (Half-Kelly): Risk %.2f%% of total account equity\n", half_kelly_pct);
    } else {
        printf("Verdict: REJECT TRADE (Reward does not justify the risk)\n");
        printf("Recommended Position Size: 0.00%% (Capital preservation)\n");
    }
    printf("===========================================\n");

    free(final_prices);
    free(md.prices); free(md.volumes); 
    free(mem.memory); free(cal_mem.memory);
    curl_global_cleanup();
    return EXIT_SUCCESS;
}