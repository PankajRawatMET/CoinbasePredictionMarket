# order_management

Reserved for the CPM **order entry** service (the second FIX gateway: its own
endpoint, its own TargetCompID and its own dictionaries) and the order state
machine above it.

Nothing is built here yet. The plan is in `docs/design-oms.md`; open questions
are in `docs/open-questions-oms.txt`. The spec extract is
`docs/spec/fix-order-entry.txt`. `market_data/` must never depend on this
container, which is why the shared session code lives in `shared/`.
