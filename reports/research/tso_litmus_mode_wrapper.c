#ifndef TSO_LITMUS_MODE
#error "TSO_LITMUS_MODE must be defined"
#endif

extern int tso_litmus_real_main(int argc, char** argv);

int main(void) {
    static char exe[] = "tso_litmus.exe";
    static char mode[] = TSO_LITMUS_MODE;
    char* argv[] = { exe, mode, 0 };
    return tso_litmus_real_main(2, argv);
}
