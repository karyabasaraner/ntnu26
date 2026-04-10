import argparse
import os
import sys

sys.path.insert(0, os.path.abspath("build"))
import core


def main():
    parser = argparse.ArgumentParser(description="Convert a legacy core MCAP log to the Foxglove-compatible format")
    parser.add_argument("input_mcap", help="Path to the legacy MCAP file")
    parser.add_argument("output_mcap", help="Path for the converted Foxglove-compatible MCAP file")
    args = parser.parse_args()

    core.convert_legacy_log_file_to_foxglove(args.input_mcap, args.output_mcap)
    print(f"Converted {args.input_mcap} -> {args.output_mcap}")


if __name__ == "__main__":
    main()
