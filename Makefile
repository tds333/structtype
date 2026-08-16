.DEFAULT_GOAL := help
SOURCE_DIR = ./src
PY_VERSIONS = 3.10 3.11 3.12 3.13 3.14 3.14t 3.15 3.15t
export UV_MANAGED_PYTHON ?= 1

##@ Build
.PHONY: build
build: ## Build
	uv build

.PHONY: docs
docs: ## build docs
	rm -rf .doctrees site
	uv run --group docs sphinx-build -d .doctrees -b html docs site --fail-on-warning

.PHONY: docs-serve
docs-serve: ## Open built docs in browser
	uv run python -m webbrowser site/index.html

.PHONY: wheels
wheels: ## build wheels
	uvx --from cibuildwheel==4.2.0 cibuildwheel

##@ Quality
.PHONY: test-cov
test-cov: build ## Run tests with coverage
	uv run pytest --cov-report=term-missing --cov-config=pyproject.toml --cov=structtype

.PHONY: test-lf
test-lf: ## Run tests in current Python
	uv run --reinstall pytest --lf

.PHONY: test-doc
test-doc: ## Run doctests
	uv run pytest --doctest-modules --pyargs structtype

.PHONY: test-all
test-all: ## Run tests in all supporte Python versions
	for py_v in $(PY_VERSIONS); do \
		uv run --isolated --reinstall-package structtype -p $$py_v pytest; \
	done

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
ASAN_RUNTIME := $(shell clang --print-file-name=libclang_rt.asan_osx_dynamic.dylib)
SANITIZE_PRELOAD := DYLD_INSERT_LIBRARIES=$(ASAN_RUNTIME)
else
ASAN_RUNTIME := $(shell gcc --print-file-name=libasan.so)
SANITIZE_PRELOAD := LD_PRELOAD=$(ASAN_RUNTIME)
endif

DEBUG_PY ?= 3.14+debug
DEBUG_VENV = .venv-debug

.PHONY: test-debug
test-debug: ## Build core with Py_DEBUG + ASan/UBSan + debug allocator and run all tests
	uv venv --clear --python $(DEBUG_PY) $(DEBUG_VENV)
	STRUCTTYPE_SANITIZE=1 uv pip install --python $(DEBUG_VENV) --reinstall --no-cache --group dev -e .
	$(SANITIZE_PRELOAD) STRUCTTYPE_ASAN_RUNTIME=$(ASAN_RUNTIME) ASAN_OPTIONS=detect_leaks=0 \
		PYTHONMALLOC=debug PYTHONFAULTHANDLER=1 PYTHONDEVMODE=1 \
		$(DEBUG_VENV)/bin/python -m pytest

.PHONY: check
check: ## Run all checks
	-uvx ty check ${SOURCE_DIR}
	uvx ruff check ${SOURCE_DIR}

.PHONY: ruff-check
ruff-check: ## Lint using ruff
	uvx ruff check ${SOURCE_DIR}

.PHONY: type-check
type-check: ## Type check with
	-uvx ty check ${SOURCE_DIR}
	uvx pyrefly check ${SOURCE_DIR}

.PHONY: format
format: ## Format files using ruff format
	uvx ruff format ${SOURCE_DIR}

##@ Benchmark
.PHONY: bench
bench: ## run benchmarks
	uv run benchmarks/bench_libs.py

##@ Utility
.PHONY: clean
clean: ## Delete all temporary files
	rm -rf .pytest_cache
	rm -rf **/.pytest_cache
	rm -rf .mypy_cache
	rm -rf .ruff_cache
	rm -rf __pycache__
	rm -rf **/__pycache__
	rm -rf build
	rm -rf dist
	rm -f .coverage

.PHONY: install
install: install-uv ## Install virtual environment
	uv sync --frozen

.PHONY: install-uv
install-uv: ## Install uv
	@command -v uv >/dev/null 2>&1 || curl -LsSf https://astral.sh/uv/install.sh | sh

.PHONY: update-uv
update-uv: ## Update uv
	uv self update

.PHONY: update-lock
update-lock: ## Update lockfile
	uv lock --upgrade

.PHONY: update-python
update-python: ## Reinstall managed Python versions to latest release
	for py_v in $(PY_VERSIONS); do \
		uv python install --reinstall $$py_v ; \
	done


.PHONY: help
help:  ## Display this help
	@awk 'BEGIN {FS = ":.*##"; printf "\nUsage:\n  make <target>\033[36m\033[0m\n"} /^[a-zA-Z_-]+:.*?##/ { printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2 } /^##@/ { printf "\n\033[1m%s\033[0m\n", substr($$0, 5) } ' $(MAKEFILE_LIST)
