#ifdef DEBUG_ENABLED
    #define DEBUG_CODE(block)  [&]() { block; }()
#else
    #define DEBUG_CODE(block)  []() {}()
#endif