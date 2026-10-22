"""roclink-hub: store roclink uplinks from MQTT in SQLite.

    roclink-hub --config /etc/roclink-hub/config.toml
"""

import argparse
import logging
import sys

from . import config, supervisor
from .process import setup_logging


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="roclink-hub", description=__doc__.splitlines()[0])
    parser.add_argument("-c", "--config", required=True, help="path to the TOML config file")
    args = parser.parse_args(argv)

    try:
        cfg = config.load(args.config)
    except config.ConfigError as e:
        print(f"roclink-hub: {e}", file=sys.stderr)
        return 2
    setup_logging(cfg.log_level)
    logging.getLogger(__name__).info("starting with %s", args.config)
    return supervisor.run(cfg)


if __name__ == "__main__":
    sys.exit(main())
