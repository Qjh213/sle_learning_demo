# Beginner SLE TX/RX demo. Reuse the proven SSAP client/server implementation,
# while keeping the application entry and tunable parameters in this directory.

set(SLE_UART_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../sle_uart)

if(DEFINED CONFIG_SLE_LEARNING_ROLE_SERVER)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_learning_demo.c
        ${SLE_UART_BASE}/sle_uart_server/sle_uart_server.c
        ${SLE_UART_BASE}/sle_uart_server/sle_uart_server_adv.c
    )
    set(HEADER_LIST ${SLE_UART_BASE}/sle_uart_server)
elseif(DEFINED CONFIG_SLE_LEARNING_ROLE_CLIENT)
    set(SOURCES_LIST
        ${CMAKE_CURRENT_SOURCE_DIR}/sle_learning_demo.c
        ${SLE_UART_BASE}/sle_uart_client/sle_uart_client.c
    )
    set(HEADER_LIST ${SLE_UART_BASE}/sle_uart_client)
endif()

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER "${PUBLIC_HEADER}" ${HEADER_LIST} ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)

