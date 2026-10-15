# Vendored schema dependency

Source: https://github.com/Tencent/rapidjson
Pinned commit: 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
Only include/rapidjson headers are retained; bin/jsonchecker is not included.
Original license is preserved in LICENSE.txt.

Local isolation transform: RAPIDJSON_ macros become LIBAI_RJ_, and the default
namespace becomes libai_schema_json. This avoids collisions with application
RapidJSON versions. No validation algorithm was changed.
