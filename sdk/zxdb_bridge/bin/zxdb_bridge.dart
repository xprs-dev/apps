// hal.db for a wapp's native tests, on the real database engine.
//
// The host answers hal_db_* with a zx store (app/lib/wapp/wapp_engine.dart:
// a ZxSqlDb per opened path, SQL in the SQLite dialect, bind parameters as a
// JSON array, rows back as a JSON array of objects). A native test is C and
// cannot link a Dart engine, so ../hal_db_zx.c starts this program and speaks
// to it one JSON line at a time:
//
//   -> {"op":"open","path":"/abs/dir/name.db"}      <- {"ok":true,"h":1}
//   -> {"op":"exec","h":1,"sql":"...","params":[...]}  <- {"ok":true}
//   -> {"op":"query","h":1,"sql":"...","params":[...]} <- {"ok":true,"rows":[...]}
//   -> {"op":"close","h":1}                         <- {"ok":true}
//   <- {"ok":false,"error":"..."}                   on any failure
//
// The store is a file, named as the host names it (`<name>.zx`, a trailing
// .sqlite3/.sqlite/.db replaced), opened with the host's options, so a test
// that closes and reopens it sees what a restarted wapp sees. Parameters and
// rows are converted exactly as the host converts them.
import 'dart:convert';
import 'dart:io';

import 'package:zx/zxdb.dart';

String _storePath(String path) {
  final slash = path.lastIndexOf('/');
  final dir = slash < 0 ? '' : path.substring(0, slash + 1);
  var name = path.substring(slash + 1).replaceFirst(RegExp(r'\.(sqlite3?|db)$'), '');
  if (!name.endsWith('.zx')) name = '$name.zx';
  return '$dir$name';
}

void main() {
  final dbs = <int, ZxSqlDb>{};
  final paths = <int, String>{};
  var next = 1;
  stdin
      // As the host reads a wapp's strings: malformed bytes become U+FFFD
      // rather than an error (wapp_engine.dart, _readUtf8).
      .transform(const Utf8Decoder(allowMalformed: true))
      .transform(const LineSplitter())
      .listen((line) {
    if (line.trim().isEmpty) return;
    Map<String, Object?> reply;
    try {
      final req = jsonDecode(line) as Map<String, dynamic>;
      final op = req['op'];
      if (op == 'open') {
        final path = _storePath(req['path'] as String);
        final slash = path.lastIndexOf('/');
        if (slash > 0) Directory(path.substring(0, slash)).createSync(recursive: true);
        // zx shares one open store per path in a process. A test that wiped
        // its data folder (rm -rf between cases) would get the deleted store
        // back, so handles to a store whose file is gone are dropped first.
        // A wapp in the app never deletes its own open store.
        if (!File(path).existsSync()) {
          for (final e in paths.entries.where((e) => e.value == path).toList()) {
            dbs.remove(e.key)?.dispose();
            paths.remove(e.key);
          }
        }
        final h = next++;
        dbs[h] = ZxSqlDb.open(path, lockWaitMs: 4000, pageSize: 16384);
        paths[h] = path;
        reply = {'ok': true, 'h': h};
      } else if (op == 'close') {
        dbs.remove(req['h'])?.dispose();
        paths.remove(req['h']);
        reply = {'ok': true};
      } else {
        final db = dbs[req['h']];
        if (db == null) throw StateError('no open store ${req['h']}');
        final sql = req['sql'] as String? ?? '';
        final p = req['params'];
        final params = p is List ? p : const <Object?>[];
        if (op == 'query') {
          final rs = db.select(sql, params);
          final cols = rs.columnNames;
          reply = {
            'ok': true,
            'rows': [
              for (final row in rs) {for (final c in cols) c: row[c]}
            ],
          };
        } else {
          db.execute(sql, params);
          reply = {'ok': true};
        }
      }
    } catch (e) {
      reply = {'ok': false, 'error': '$e'};
    }
    stdout.writeln(jsonEncode(reply));
  }, onDone: () {
    for (final db in dbs.values) {
      db.dispose();
    }
  });
}
