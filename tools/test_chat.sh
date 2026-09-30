#!/usr/bin/env bash
# Secure chat (tools/test_chat.py) against vanilla and this server, online with
# enforce-secure-profile, both using the stand-in services of tools/mock_services.py.
#   tools/test_chat.sh
#   SERVERS=cmc tools/test_chat.sh
TEST=test_chat.py ENFORCE=true exec "$(dirname "$0")/test_online.sh" "$@"
