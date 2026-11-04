.PHONY: test typecheck build check install-hooks mock-upstream screens screens-test

test:
	npm test

typecheck:
	npm run typecheck

build:
	npm run build

check: test typecheck build

install-hooks:
	./scripts/install-hooks.sh

mock-upstream:
	npm run mock-upstream -- --scenario normal --port 8787

screens:
	./scripts/render-ui-screens.sh

screens-test: screens
	python3 tools/uisim/test_outputs.py docs/screens
