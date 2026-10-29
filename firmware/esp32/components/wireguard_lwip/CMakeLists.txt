# Vendored with TailTiny (../TAILTINY.md). Compiled only when CONFIG_HG_TAILNET
# is on; the requirements stay unconditional (resolved before the sdkconfig exists).
set(WG_SRCS)
if(CONFIG_HG_TAILNET)
    list(APPEND WG_SRCS
        "src/wireguard.c"
        "src/wireguardif.c"
        "src/crypto.c"
        "src/wireguard-platform-esp32.c"
        "src/crypto/refc/blake2s.c"
        "src/crypto/refc/chacha20.c"
        "src/crypto/refc/chacha20poly1305.c"
        "src/crypto/refc/poly1305-donna.c"
        "src/crypto/refc/x25519.c")
endif()

idf_component_register(
    SRCS ${WG_SRCS}
    INCLUDE_DIRS
        "src"
        "src/crypto/refc"
    REQUIRES
        lwip
        esp_timer
)

if(CONFIG_HG_TAILNET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        WIREGUARD_CRYPTO_REFC=1
    )
    # GCC 15: the protocol names (CONSTRUCTION, IDENTIFIER, LABEL_*) are byte arrays
    # initialised from strings of exactly their length, deliberately without the NUL.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unterminated-string-initialization)

    # Disable stringop-overread warning for x25519.c (false positive in GCC array bounds check)
    set_source_files_properties(
        src/crypto/refc/x25519.c
        PROPERTIES COMPILE_FLAGS "-Wno-stringop-overread"
    )
endif()
