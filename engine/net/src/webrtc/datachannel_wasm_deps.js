// Emscripten JS library linked with datachannel-wasm's own two (webrtc.js,
// websocket.js). Their $WEBRTC and $WEBSOCKET helpers call _malloc and _free
// to hand strings and buffers to C++, but declare no dependency on them, so
// Emscripten (4+) neither keeps nor exports malloc/free and the first
// callback into C++ throws "ReferenceError: _malloc is not defined". The
// browser-free paths the node tests run never reach that code; the first
// browser run (apps/net-probe, tools/web/check_webrtc.py) did.
//
// A forced placeholder symbol that depends on malloc and free pulls both in
// whenever kfm_net_webrtc is linked. Adding the deps to $WEBRTC/$WEBSOCKET
// directly does not work: their autoAddDeps() replaces those keys when the
// libraries are merged. Unlike -sEXPORTED_FUNCTIONS on the link, this leaves
// every executable free to set its own exports.
addToLibrary({
  $kfmDatachannelAllocDeps__deps: ['malloc', 'free'],
  $kfmDatachannelAllocDeps__force: true,
  $kfmDatachannelAllocDeps: () => {},
});
