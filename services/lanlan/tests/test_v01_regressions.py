"""Socket-free request/SQLite regressions for the first device-test release."""
import json
import tempfile
import unittest
from unittest.mock import patch
from datetime import timedelta

from . import bootstrap  # noqa: F401
from lanlan import admin, api, auth, db, model
from lanlan.config import Config


class InitialReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.config = Config(db_path=self.temp.name + '/test.db', pbkdf2_iterations=1000)
        self.connection = db.connect(self.config.db_path)
        self.addCleanup(self.connection.close)
        db.initialize(self.connection)
        info = admin.initialize_database(self.connection, self.config,
            passwords={'hehe': 'test-password', 'yangyang': 'test-password'})
        self.user = self.connection.execute("SELECT * FROM users WHERE username='hehe'").fetchone()
        with db.transaction(self.connection):
            self.session, self.csrf = auth.create_session(self.connection, self.user['id'], 30)
            _, self.token = auth.create_device(self.connection, info['family_id'], 'test-device')
        self.application = api.Api(self.config)

    def call(self, method, path, body=None, device=False, expected=200):
        headers = {'Host': 'localhost', 'Origin': 'http://localhost',
                   'X-Lanlan-CSRF': self.csrf, 'Cookie': 'lanlan_session=' + self.session}
        if device:
            headers = {'Authorization': 'Bearer ' + self.token}
        with (db.read_transaction if method == 'GET' else db.transaction)(self.connection):
            response = self.application.handle(api.Request(method, '/api/v1' + path,
                headers, api.json_bytes(body or {})), self.connection)
        self.assertEqual(response.status, expected, response.body)
        return json.loads(response.body)

    def create(self, index=0, **extra):
        body = {'category': 'meal', 'occurred_at': model.format_utc(
            model.now_utc() - timedelta(minutes=100-index)), **extra}
        return self.call('POST', '/records', body)['record']

    def snapshot(self):
        return self.call('GET', '/sync/snapshot?latest=1&limit=40', device=True)

    def test_latest_window_and_long_utf8_notes_fit_device(self):
        records = [self.create(i, note='澡🐕'*100) for i in range(45)]
        page = self.snapshot()
        self.assertFalse(page['has_more'])
        self.assertEqual([r['id'] for r in page['records']], [r['id'] for r in records[-40:]])
        self.assertLess(len(api.json_bytes(page)), 16384)
        self.assertGreater(len(page['records'][0]['note'].encode()), 48)
        self.assertEqual(self.call('GET', '/records/' + records[-1]['id'])['record']['note'], '澡🐕'*100)
        self.assertEqual(len(page['reminders']), 7)

    def test_escaped_notes_shrink_only_device_window(self):
        records = [self.create(i, category='other', custom_name='自定义记录名称测试名称字',
                               note='\x01'*200) for i in range(40)]
        page = self.snapshot()
        self.assertGreater(len(page['records']), 0)
        self.assertLess(len(page['records']), 40)
        self.assertLessEqual(len(api.json_bytes(page)), 15 * 1024)
        self.assertEqual(page['records'][-1]['id'], records[-1]['id'])
        self.assertEqual(page['window_count'], len(page['records']))
        self.assertEqual(self.connection.execute('SELECT COUNT(*) FROM records').fetchone()[0], 40)

    def test_write_during_snapshot_is_delivered_after_its_cursor(self):
        for i in range(41):
            self.create(i)
        reminder = self.call('GET', '/reminders')['reminders'][0]
        # Run a second SQLite writer after this snapshot has acquired its read
        # view but before its cursor is computed. WAL lets it commit concurrently.
        compact = model.record_to_compact
        inserted = []
        def interleave(row):
            if not inserted:
                other = db.connect(self.config.db_path)
                old = self.connection
                self.connection = other
                try:
                    inserted.append(self.create(60))
                    self.call('PATCH', '/reminders/' + reminder['id'],
                              {'enabled': True, 'time_local': '09:00'})
                finally:
                    self.connection = old
                    other.close()
            return compact(row)
        # Isolate the read-view contract from the authentication heartbeat write,
        # which otherwise serializes writers before snapshot construction.
        with patch.object(auth, 'touch_device'), patch.object(model, 'record_to_compact', side_effect=interleave):
            page = self.snapshot()
        self.assertNotIn(inserted[0]['id'], [r['id'] for r in page['records']])
        self.assertFalse(next(r['en'] for r in page['reminders'] if r['id'] == reminder['id']))
        changes = self.call('GET', '/sync/changes?cursor=' + str(page['cursor']), device=True)
        self.assertIn(inserted[0]['id'], [r['id'] for r in changes['records']])
        self.assertTrue(next(r['en'] for r in changes['reminders'] if r['id'] == reminder['id']))

    def test_lost_response_modified_retry_preserves_both_intents(self):
        body = {'category': 'meal', 'note': 'first', 'client_request_id': 'retry-key'}
        first = self.call('POST', '/records', body)['record']
        replay = self.call('POST', '/records', body)
        self.assertTrue(replay['idempotent_replay'])
        conflict = self.call('POST', '/records', {**body, 'note': 'corrected'}, expected=409)
        self.assertEqual(conflict['error']['code'], 'idempotency_conflict')
        self.assertEqual(conflict['record']['id'], first['id'])
        edited = self.call('PATCH', '/records/' + first['id'],
                          {**body, 'occurred_at': first['occurred_at'], 'note': 'corrected', 'expected_version': first['version']})
        self.assertEqual(edited['record']['note'], 'corrected')
        self.assertEqual(self.connection.execute('SELECT COUNT(DISTINCT id) FROM records').fetchone()[0], 1)

    def test_empty_snapshot_and_same_request_after_edit(self):
        self.assertEqual(self.snapshot()['records'], [])
        body = {'category': 'water', 'client_request_id': 'edit-key'}
        first = self.call('POST', '/records', body)['record']
        self.call('PATCH', '/records/' + first['id'], {**body, 'occurred_at': first['occurred_at'], 'expected_version': 1, 'note': 'updated'})
        replay = self.call('POST', '/records', body)
        self.assertEqual(replay['record']['version'], 2)
        self.assertTrue(replay['idempotent_replay'])


if __name__ == '__main__':
    unittest.main()
