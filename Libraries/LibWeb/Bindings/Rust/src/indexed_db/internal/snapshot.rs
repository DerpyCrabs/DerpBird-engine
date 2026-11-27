// SPDX-License-Identifier: BSD-2-Clause

//! Profile format for IndexedDB snapshots. Strings preserve every UTF-16 code unit,
//! including unpaired surrogates. Values are structured-clone storage records.
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::Vm;

use super::algorithms::KeyPath;
use super::database::DatabaseRef;
use super::key::{Key, KeyType, KeyValue};

const MAGIC: &[u8] = b"IDB\0\x01";
const STORE_MAGIC: &[u8] = b"IDB\0\x02";
pub(crate) fn catalog_entry(bytes: &[u8]) -> Option<(Utf16String, u64)> {
    let (version, name) = bytes
        .strip_prefix(MAGIC)
        .or_else(|| bytes.strip_prefix(STORE_MAGIC))?
        .split_at_checked(8)?;
    if !name.len().is_multiple_of(2) {
        return None;
    }
    let units: Vec<_> = name
        .chunks_exact(2)
        .map(|unit| u16::from_le_bytes([unit[0], unit[1]]))
        .collect();
    Some((
        Utf16String::from_utf16(&units),
        u64::from_le_bytes(version.try_into().ok()?),
    ))
}

const MAX_KEY_DEPTH: usize = 256;

#[derive(Default)]
struct Writer(Vec<u8>);

impl Writer {
    fn count(&mut self, n: usize) -> Option<()> {
        self.0.extend(u32::try_from(n).ok()?.to_le_bytes());
        Some(())
    }

    fn bytes(&mut self, bytes: &[u8]) -> Option<()> {
        self.count(bytes.len())?;
        self.0.extend_from_slice(bytes);
        Some(())
    }

    fn string(&mut self, value: &Utf16String) -> Option<()> {
        self.count(value.length_in_code_units())?;
        for unit in value.to_utf16().iter() {
            self.0.extend(unit.to_le_bytes());
        }
        Some(())
    }

    fn path(&mut self, path: Option<&KeyPath>) -> Option<()> {
        match path {
            None => self.0.push(0),
            Some(KeyPath::String(name)) => {
                self.0.push(1);
                self.string(name)?;
            }
            Some(KeyPath::Sequence(names)) => {
                self.0.push(2);
                self.count(names.len())?;
                for name in names {
                    self.string(name)?;
                }
            }
        }
        Some(())
    }

    fn key(&mut self, key: &Key, depth: usize) -> Option<()> {
        if depth > MAX_KEY_DEPTH {
            return None;
        }
        match key.value() {
            KeyValue::Double(value) => {
                self.0.push(match key.r#type() {
                    KeyType::Number => 0,
                    KeyType::Date => 1,
                    _ => return None,
                });
                self.0.extend(value.to_le_bytes());
            }
            KeyValue::String(value) if key.r#type() == KeyType::String => {
                self.0.push(2);
                self.string(value)?;
            }
            KeyValue::Bytes(value) => {
                self.0.push(3);
                self.bytes(value)?;
            }
            KeyValue::Keys(values) => {
                self.0.push(4);
                self.count(values.len())?;
                for value in values {
                    self.key(value, depth + 1)?;
                }
            }
            _ => return None,
        }
        Some(())
    }
}

/// The header is stored once; each framed store becomes a separately replaceable SQLite row.
pub(crate) fn encode_stores(
    vm: &Vm,
    database: DatabaseRef,
    stores: &[super::object_store::ObjectStoreRef],
    replace_all: bool,
) -> Option<Vec<u8>> {
    let mut writer = Writer(STORE_MAGIC.to_vec());
    writer.0.extend(database.version().to_le_bytes());
    writer.count(database.object_stores(vm).values().len())?;
    writer.0.push(u8::from(replace_all));
    for store in stores {
        if !replace_all && store.mutation_log_position() == 0 {
            continue;
        }
        let frame = writer.0.len();
        writer.count(0)?;
        writer.string(&store.name())?;
        writer.path(store.key_path())?;
        let generator = if store.uses_a_key_generator() {
            store.key_generator().current_number()
        } else {
            0
        };
        writer.0.extend(generator.to_le_bytes());
        let records = store.records();
        writer.count(records.len())?;
        for record in records.iter() {
            writer.key(&record.key, 0)?;
            writer.bytes(&record.value)?;
        }
        let indexes = store.indexes(vm);
        writer.count(indexes.values().len())?;
        for index in indexes.values().iter() {
            writer.string(&index.name())?;
            writer.path(Some(index.key_path()))?;
            writer
                .0
                .push(u8::from(index.unique()) | (u8::from(index.multi_entry()) << 1));
            let records = index.records();
            writer.count(records.len())?;
            for record in records.iter() {
                writer.key(&record.key, 0)?;
                writer.key(&record.value, 0)?;
            }
        }
        let length = u32::try_from(writer.0.len() - frame - 4).ok()?;
        writer.0[frame..frame + 4].copy_from_slice(&length.to_le_bytes());
    }
    Some(writer.0)
}

struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, size: usize) -> Option<&'a [u8]> {
        let (value, rest) = self.0.split_at_checked(size)?;
        self.0 = rest;
        Some(value)
    }

    fn byte(&mut self) -> Option<u8> {
        Some(self.take(1)?[0])
    }

    fn count(&mut self) -> Option<usize> {
        let n = u32::from_le_bytes(self.take(4)?.try_into().ok()?) as usize;
        (n <= self.0.len()).then_some(n)
    }

    fn bytes(&mut self) -> Option<&'a [u8]> {
        let n = self.count()?;
        self.take(n)
    }

    fn string(&mut self) -> Option<Utf16String> {
        let n = self.count()?;
        let bytes = self.take(n.checked_mul(2)?)?;
        let units: Vec<u16> = bytes
            .chunks_exact(2)
            .map(|p| u16::from_le_bytes([p[0], p[1]]))
            .collect();
        Some(Utf16String::from_utf16(&units))
    }

    fn key(&mut self, depth: usize) -> Option<Rc<Key>> {
        if depth > MAX_KEY_DEPTH {
            return None;
        }
        match self.byte()? {
            tag @ (0 | 1) => {
                let n = f64::from_le_bytes(self.take(8)?.try_into().ok()?);
                if n.is_nan() {
                    return None;
                }
                Some(if tag == 0 {
                    Key::create_number(n)
                } else {
                    Key::create_date(n)
                })
            }
            2 => Some(Key::create_string(self.string()?)),
            3 => Some(Key::create_binary(self.bytes()?.to_vec())),
            4 => {
                let n = self.count()?;
                let keys = (0..n).map(|_| self.key(depth + 1)).collect::<Option<Vec<_>>>()?;
                Some(Key::create_array(keys))
            }
            _ => None,
        }
    }
}

impl Reader<'_> {
    fn integer(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.take(8)?.try_into().ok()?))
    }

    fn path(&mut self) -> Option<Option<KeyPath>> {
        match self.byte()? {
            0 => Some(None),
            1 => Some(Some(KeyPath::String(self.string()?))),
            2 => {
                let n = self.count()?;
                let names = (0..n).map(|_| self.string()).collect::<Option<Vec<_>>>()?;
                Some(Some(KeyPath::Sequence(names)))
            }
            _ => None,
        }
    }
}

/// Populate a newly allocated database. The caller publishes it only after
/// the complete snapshot has been consumed.
pub(crate) fn restore(vm: &Vm, database: DatabaseRef, snapshot: &[u8]) -> Option<()> {
    use super::index::Index;
    use super::object_store::ObjectStore;
    use crate::indexed_db::idb_record::{IndexRecord, ObjectStoreRecord};

    let mut reader = Reader(snapshot);
    let magic = reader.take(MAGIC.len())?;
    if magic != MAGIC && magic != STORE_MAGIC {
        return None;
    }
    let version = reader.integer()?;
    for _ in 0..reader.count()? {
        let name = reader.string()?;
        if database.object_store_with_name(&name).is_some() {
            return None;
        }
        let key_path = reader.path()?;
        let generator = reader.integer()?;
        let store = ObjectStore::create(vm, database, name, generator != 0, key_path);
        if generator != 0 {
            store.key_generator().set(generator);
        }
        for _ in 0..reader.count()? {
            let key = reader.key(0)?;
            let value = Rc::from(reader.bytes()?);
            store.store_a_record(vm, ObjectStoreRecord { key, value });
        }
        for _ in 0..reader.count()? {
            let name = reader.string()?;
            if store.index_set_get(&name).is_some() {
                return None;
            }
            let path = reader.path()??;
            let flags = reader.byte()?;
            if flags > 3 {
                return None;
            }
            let index = Index::create(vm, store, &name, &path, flags & 1 != 0, flags & 2 != 0);
            for _ in 0..reader.count()? {
                let key = reader.key(0)?;
                let value = reader.key(0)?;
                Index::store_a_record(index, vm, &IndexRecord { key, value });
            }
        }
    }
    if !reader.0.is_empty() {
        return None;
    }
    database.set_version(version);
    Some(())
}
