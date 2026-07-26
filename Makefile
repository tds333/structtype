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
	uv run --group docs sphinx-build -d .doctrees -b html docs site --fail-on-warning

##@ Quality
.PHONY: test-cov
test-cov: ## Run tests with coverage
	uv run pytest --cov-report=term-missing --cov-config=pyproject.toml --cov=structtype

.PHONY: test-lf
test-lf: ## Run tests in current Python
	uv run --reinstall pytest --lf

.PHONY: test-all
test-all: ## Run tests in all supporte Python versions
	for py_v in $(PY_VERSIONS); do \
		uv run --isolated -p $$py_v pytest; \
	done

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
	uv run --group bench benchmarks/bench_compare.py
	uv run --group bench benchmarks/bench_libs.py

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
