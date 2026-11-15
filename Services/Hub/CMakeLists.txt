# QObject-based DerpBird services share Core's event loop in both processes.
add_library(derpbird_qt_event_loop STATIC
    ${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQt.cpp
    ${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQtEventTarget.cpp
    ${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQtEventTarget.h
)
set_target_properties(derpbird_qt_event_loop PROPERTIES AUTOMOC ON)
target_link_libraries(derpbird_qt_event_loop PUBLIC Qt6::Core LibCore)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

set(SOURCES)
compile_ipc(HubClient.ipc HubClientEndpoint.h)
compile_ipc(HubServer.ipc HubServerEndpoint.h)

# The messages between the Hub and the UI, which both of them need.
add_library(hubipc STATIC ${SOURCES})
target_include_directories(hubipc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubipc PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(hubipc PRIVATE LibCore LibGfx LibHTTP LibIPC LibURL LibWebView)

set(SOURCES
    Application.cpp
    ConnectionFromUI.cpp
    DerpBird.cpp
    Proxies.cpp
    View.cpp
)

add_library(hubservice STATIC ${SOURCES})
target_compile_definitions(hubservice PRIVATE QT_NO_KEYWORDS)
add_dependencies(hubservice hubipc)

target_include_directories(hubservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(hubservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(hubservice PRIVATE LibCore LibGfx LibHTTP LibIPC LibRequests LibURL LibWebView hubipc derpbird_qt_event_loop Qt6::Gui LibRegex LibImageDecoderClient LibDatabase)

add_executable(Hub main.cpp)
target_include_directories(Hub PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)
target_link_libraries(Hub PRIVATE hubservice hubipc LibCore LibHTTP LibIPC LibMain LibRequests LibURL LibWebView)

ladybird_sign_helper_process(Hub)
