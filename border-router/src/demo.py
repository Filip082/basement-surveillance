"""Print example whole-message DATA + END frames for --simulate-hex."""

import json

from .protocol import Kind, decode_frame, encode_control, encode_message


def main() -> None:
    """Print version 1 DATA frames followed by END for entry, illustrative image bytes and
    telemetry.

    Args:
        None.

    Returns:
        None: This operation returns no value.
    """
    examples = [
        (Kind.ENTRY, 1, json.dumps({"event": "person_entered"}).encode()),
        (Kind.IMAGE, 2, b"\xff\xd8" + b"photo" * 120 + b"\xff\xd9"),
        (Kind.TELEMETRY, 3, b'{"battery_mv":3860,"temp_c":19.3}'),
    ]
    for kind, message_id, data in examples:
        frames = encode_message(kind, 1, message_id, data)
        for f in frames:
            print(f.hex())
        print(encode_control(Kind.END, decode_frame(frames[0])).hex())


if __name__ == "__main__":
    main()
