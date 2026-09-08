// The dedicated server binary.
//
// Three lines on purpose. Everything it does lives in ov_server, so that the
// client can host exactly the same server on a thread and talk to it over a
// real socket — which is what makes "the solo game is multiplayer" a fact
// about the code rather than a slogan.
#include "ov/server/server.hpp"

int main(int argc, char** argv) {
    return ov::server::run(argc, argv);
}
