#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#define lseek _lseek
#define ltell _ltell
#define read _read
#define write _write
#else
#include <unistd.h>
#endif

#define SECTOR_SIZE 32 //512
#define NAME_LENGTH 10 //100
#define NAME_LENGTH1 (NAME_LENGTH + 1)

typedef unsigned char byte;
typedef byte Sector[SECTOR_SIZE];

class AbstractBlockDevice
{
public:
	virtual bool writeBlock(int sector, const Sector &data) = 0;
	virtual bool readBlock(int sector, Sector &data) = 0;
};

class DirectoryEntry
{
public:
	bool writeHeaderSector(Sector &sector)
	{
		fprintf(stderr, "writeHeaderSector alloc: %ld, len: %ld, name_len: %ld\n", _allocated, _length, _name_len);
		sector[0] = 'S';
		sector[1] = 'D';
		sector[2] = 'f';
		sector[3] = 's';
		sector[4] = (byte)((_allocated >> 16) & 0xff);
		sector[5] = (byte)((_allocated >> 8) & 0xff);
		sector[6] = (byte)(_allocated & 0xff);
		sector[7] = (byte)((_length >> 16) & 0xff);
		sector[8] = (byte)((_length >> 8) & 0xff);
		sector[9] = (byte)(_length & 0xff);
		const char *s = _name;
		_name_len = 0;
		for (; _name_len < NAME_LENGTH && *s != '\0'; _name_len++, s++)
			sector[10 + _name_len] = *s;
		sector[10 + _name_len] = '\0';
		unsigned short check_sum = calc_checksum(sector, 10 + _name_len);
		sector[11 + _name_len] = (byte)((check_sum >> 8) & 0xff);
		sector[12 + _name_len] = (byte)(check_sum & 0xff);
		_used = sectorsNeeded(_name_len, _length); // not really needed
		return true;
	}
	bool readHeaderSector(const Sector &sector)
	{
		if (sector[0] != 'S' || sector[1] != 'D' || sector[2] != 'f' || sector[3] != 's')
			return false;
		_allocated = ((unsigned long)sector[4] << 16) | ((unsigned long)sector[5] << 8) | sector[6];
		_length = ((unsigned long)sector[7] << 16) | ((unsigned long)sector[8] << 8) | sector[9];
		_name_len = 0;
		for (; _name_len < NAME_LENGTH1; _name_len++)
		{
			char ch = sector[10 + _name_len];
			_name[_name_len] = ch;
			if (ch == '\0')
				break;
		}
		if (_name_len == NAME_LENGTH1)
			return false;
		_used = sectorsNeeded(_name_len, _length);
		unsigned short check_sum = calc_checksum(sector, 10 + _name_len);
		//fprintf(stderr, "readHeaderSector alloc: %ld, len: %ld, name_len: %ld |%s|\n", _allocated, _length, _name_len, _name);
		return (((unsigned long)sector[11 + _name_len] << 8) | sector[12 + _name_len]) == check_sum;
	}
	unsigned short startOfData() { return _name_len + 13; }
	unsigned long startSector() { return _start_sector; }
	const char* name() { return _name; }
	unsigned short nameLength() { return _name_len; }
	unsigned long length() { return _length; }
	unsigned long allocated() { return _allocated; }
	unsigned long used() { return _used; }
	unsigned long unused() { return _allocated - _used; }
	static unsigned long sectorsNeeded(unsigned short name_len, unsigned long length)
	{
		if (name_len == 0 && length == 0)
			return 0;
		return (13 + (unsigned long)name_len + length + SECTOR_SIZE-1)/SECTOR_SIZE;
	}
	void clearName()
	{
		_name[0] = '\0';
		_name_len = 0;
		_used = sectorsNeeded(_name_len, _length);
	}
	void setLength(unsigned long length) { _length = length; _used = sectorsNeeded(_name_len, _length); }
	void setAllocated(unsigned long allocated) { _allocated = allocated; }
	void addAllocated(unsigned long allocated) { _allocated += allocated; }
	
	class ReadStream
	{
	public:
		ReadStream(AbstractBlockDevice& blockDevice) : _blockDevice(blockDevice), _more(false) {}
		Sector &open(DirectoryEntry& directoryEntry)
		{
			_cur_sector = directoryEntry.startSector();
			_length = directoryEntry.length();
			_first_unused_sector = _cur_sector + directoryEntry.used();
			_pos_in_cur_sector = directoryEntry.startOfData();
			_more = _length > 0;
			_pos = 0;
			return _sector;
		}
		bool more() { return _more; }
		byte value() { return _sector[_pos_in_cur_sector]; }
		void next()
		{
			if (++_pos >= _length)
			{
				_more = false;
				return;
			}
			if (++_pos_in_cur_sector >= SECTOR_SIZE)
			{
				++_cur_sector;
				if (_cur_sector >= _first_unused_sector)
				{
					fprintf(stderr, "Reading beyond used sectors at %ld\n", _cur_sector);
					_more = false;
					return;
				}
				if (!_blockDevice.readBlock(_cur_sector, _sector))
				{
					fprintf(stderr, "readBlock failed for sector %ld\n", _cur_sector);
					_more = false;
					return;
				}
				_pos_in_cur_sector = 0;
			}
		}
	private:
		AbstractBlockDevice& _blockDevice;
		long _length;
		byte _value;
		Sector _sector;
		bool _more;
		unsigned short _pos_in_cur_sector;
		unsigned short _pos;
		unsigned long _cur_sector;
		unsigned long _first_unused_sector;
	};				

protected:
	unsigned long _start_sector;
	char _name[NAME_LENGTH1];
	unsigned short _name_len;
	unsigned long _length;
	unsigned long _allocated;
	unsigned long _used;
private:
	short calc_checksum(const byte *data, long len)
	{
		unsigned short value = 3456;
		for (int i = 0; i < len; i++)
			value = value / 7 + value * 12 + data[i];
		return value;
	}
};

class AbstractDirectoryIterator : public DirectoryEntry
{
public:
	AbstractDirectoryIterator(AbstractBlockDevice &blockDevice) : _blockDevice(blockDevice), _more(false) {}
	virtual void init() = 0;
	bool more() { return _more; }
	virtual void next() = 0;
	virtual void getSector(Sector &sector) = 0;
	AbstractBlockDevice &blockDevice() { return _blockDevice; }
	virtual void remove() = 0;
	virtual void openModifyHeader(unsigned long sector) = 0;
	virtual void clearName() = 0;
	virtual void setLength(unsigned long length) = 0;
	virtual void setAllocated(unsigned long allocated) = 0;
	virtual void openWrite(unsigned long sector, const char*name, unsigned long length, unsigned long allocated) = 0;
	virtual void append(byte data) = 0;
	virtual void close() = 0; // Post condition _start_sector point to next sector after last write 

protected:
	AbstractBlockDevice &_blockDevice;
	bool _more;
};

class SDFileSystem
{
public:
	SDFileSystem(AbstractDirectoryIterator &directoryIterator) : _directoryIterator(directoryIterator) {}
	class ReadStream
	{
	public:
		ReadStream(SDFileSystem &fs, const char* name) : _fs(fs), _name(name), _data_read_stream(fs.directoryIterator().blockDevice())
		{
			_found = false;
			for (_fs.directoryIterator().init(); _fs.directoryIterator().more(); _fs.directoryIterator().next())
			{
				fprintf(stderr, "entry %s\n", _fs.directoryIterator().name());
				if (strcmp(_fs.directoryIterator().name(), name) == 0)
				{
					fprintf(stderr, "Found %s\n", name);
					_found = true;
					_length = _fs.directoryIterator().length();
					_fs.directoryIterator().getSector(_data_read_stream.open(_fs.directoryIterator()));
					return;
				}
			}
			fprintf(stderr, "Did not find %s\n", name);
		}
		bool found() { return _found; }
		bool more() { return _data_read_stream.more(); }
		byte value() { return _data_read_stream.value(); }
		void next() { _data_read_stream.next(); }
	private:
		SDFileSystem &_fs;
		bool _found;
		const char* _name;
		unsigned long _length;
		DirectoryEntry::ReadStream _data_read_stream;
	};
	bool writeFile(const char* name, byte *data, long length)
	{
		unsigned long sectors_needed = DirectoryEntry::sectorsNeeded(strlen(name), length);
		fprintf(stderr, "writeFile %s, sectors needed %ld\n", name, sectors_needed); 
		bool existing = false;
		bool selected = false;
		unsigned long selected_sector;
		unsigned long selected_used;
		unsigned long selected_allocated;
		for (_directoryIterator.init(); _directoryIterator.more(); _directoryIterator.next())
		{
			fprintf(stderr, "compare names %s %s\n", _directoryIterator.name(), name);
			if (!existing && strcmp(_directoryIterator.name(), name) == 0)
			{
				fprintf(stderr, "  Found file with same name, with %ld allocated\n", _directoryIterator.allocated());
				existing = true;
				if (sectors_needed <= _directoryIterator.allocated())
				{
					fprintf(stderr, "  Space enough\n");
					// new version of file, still fits at current location
					selected = true;
					selected_sector = _directoryIterator.startSector();
					selected_used = 0; // -- because we can overwrite it
					selected_allocated = _directoryIterator.allocated();
					break;
				}
				else
				{
					fprintf(stderr, "  Space not enough, set empty current location\n");
					_directoryIterator.remove();
					if (selected) fprintf(stderr, "   %ld %ld\n",  _directoryIterator.startSector(), selected_sector);
					if (selected && _directoryIterator.startSector() == selected_sector)
					{
						selected_allocated = _directoryIterator.allocated();
						fprintf(stderr, "  Adjusted allocated to %ld", selected_allocated);
					}
					// new version does not fit at current location: set empty
					//_directoryIterator.openModifyHeader(_directoryIterator.startSector());
					//_directoryIterator.clearName();
					//_directoryIterator.setLength(0);
					//_directoryIterator.close();
				}
			}
			if (sectors_needed <= _directoryIterator.unused())
			{
				fprintf(stderr, "  Found some entry with enough space %ld\n", _directoryIterator.unused());
				if (!selected || _directoryIterator.unused() < selected_allocated)
				{
					fprintf(stderr, "  Select it\n");
					selected = true;
					selected_sector = _directoryIterator.startSector();
					selected_used = _directoryIterator.used();
					selected_allocated = _directoryIterator.allocated();
				}
			}
		}
		// If no sector has been selected, allocate at the end of the device
		if (!selected)
		{
			fprintf(stderr,"  append at the end\n");
			selected_sector = _directoryIterator.startSector();
			selected_used = 0;
			selected_allocated = sectors_needed;
		}
		if (selected_used > 0)
		{
			fprintf(stderr, "  Selected already has some space used: split in two\n");
			_directoryIterator.openModifyHeader(selected_sector);
			unsigned long total_allocated = _directoryIterator.allocated();
			_directoryIterator.setAllocated(_directoryIterator.used());
			_directoryIterator.close();
			selected_sector += _directoryIterator.allocated();
			selected_used = 0;
			selected_allocated = total_allocated - _directoryIterator.allocated();
		}
		fprintf(stderr, "  Write data\n");
		_directoryIterator.openWrite(selected_sector, name, length, selected_allocated);
		for (int i = 0; i < length; i++)
			_directoryIterator.append(data[i]);
		_directoryIterator.close();
		return true;
	}
	
	AbstractDirectoryIterator &directoryIterator() { return _directoryIterator; }
	
private:
	AbstractDirectoryIterator &_directoryIterator;
};


/********** Implementation for AbstractBlockDevice ***********/

class FileBlockDevice : public AbstractBlockDevice
{
public:
	FileBlockDevice(int fh) : _fh(fh) {}
	bool writeBlock(int sector, const Sector &data)
	{
		fprintf(stderr, "Log: writeBlock(%ld) :", sector);
		for (int i = 0; i < SECTOR_SIZE; i++)
			fprintf(stderr, " %02X", (unsigned short)data[i]);
		lseek(_fh, ((long)sector) * SECTOR_SIZE, SEEK_SET);
		//fprintf(stderr, "[%ld]", ltell(_fh));
		size_t size = write(_fh, data, SECTOR_SIZE);
		bool correct = size == SECTOR_SIZE;
		fprintf(stderr, " %s\n", correct ? "correct" : "failed");
		return correct;
	}
	bool readBlock(int sector, Sector &data)
	{
		//fprintf(stderr, "Log: readBlock(%ld) :", sector);
		int r = lseek(_fh, ((long)sector) * SECTOR_SIZE, SEEK_SET);
		for (int i = 0; i < SECTOR_SIZE; i++)
			data[i] = 0;
		//fprintf(stderr, "[%ld]", ltell(_fh));
		size_t size = read(_fh, data, SECTOR_SIZE);
		bool correct = size == SECTOR_SIZE;
		//for (int i = 0; i < SECTOR_SIZE; i++)
		//	fprintf(stderr, " %02X", (unsigned short)data[i]);
		//fprintf(stderr, " %d %ld %s\n", r, size, correct ? "correct" : "failed");
		//if (!correct)
		//	fprintf(stderr, "Error: %d\n", ferror(_f));
		return correct;
	}
private:
	int _fh;
};


/************* Implementations for AbstractDirectoryIterator ************/

class RawDirectoryIterator : public AbstractDirectoryIterator
{
public:
	RawDirectoryIterator(AbstractBlockDevice &blockDevice)
	  : AbstractDirectoryIterator(blockDevice),
		_open_for_write(false), _header_modified(false), _write_pos(0), _valid_previous_sector(false) {}
	virtual void init()
	{
		_next_sector = 0;
		_valid_previous_sector = false;
		next();
	}	
	virtual void next()
	{
		if (_next_sector > 0)
		{
			_previous_sector = _start_sector;
			_valid_previous_sector = true;
		}
		_start_sector = _next_sector;
		
		_more = false;
		if (!_blockDevice.readBlock(_start_sector, _sector))
			return;
		if (readHeaderSector(_sector))
		{
			_more = true;
			_next_sector += _allocated;
		}
	}
	virtual void getSector(Sector &sector)
	{
		memcpy(sector, _sector, SECTOR_SIZE);
	}
	virtual void remove()
	{
		if (_valid_previous_sector)
		{
			unsigned long allocated = _allocated;
			_start_sector = _previous_sector;
			_blockDevice.readBlock(_start_sector, _sector);
			readHeaderSector(_sector);
			_allocated += allocated;
			writeHeaderSector(_sector);
			_blockDevice.writeBlock(_start_sector, _sector);		
			_valid_previous_sector = false;
		}
		else
		{
			// We make the current empty
			_name[0] = '\0';
			_name_len = 0;
			_length = 0;
			writeHeaderSector(_sector);
			_blockDevice.writeBlock(_start_sector, _sector);
		}
	}
	virtual void openModifyHeader(unsigned long sector)
	{
		if (sector != _start_sector)
		{
			_valid_previous_sector = false;
			_start_sector = sector;
			if (!_blockDevice.readBlock(_start_sector, _sector))
				return;
			if (!readHeaderSector(_sector))
				return;
		}
		_open_for_write = true;
		_header_modified = false;
		_write_pos = 0;
	}
	virtual void clearName()
	{
		if (!_open_for_write)
			return;
		_name[0] = '\0';
		_name_len = 0;
		_header_modified = true;
	}
	virtual void setLength(unsigned long length)
	{
		if (!_open_for_write)
			return;
		_length = length;
		_header_modified = true;
	}
	virtual void setAllocated(unsigned long allocated)
	{
		if (!_open_for_write)
			return;
		_allocated = allocated;
		_header_modified = true;
	}
	virtual void openWrite(unsigned long sector, const char *name, unsigned long length, unsigned long allocated)
	{
		_valid_previous_sector = false;
		strcpy(_name, name);
		_name_len = strlen(name);
		_start_sector = sector;
		_allocated = allocated;
		_length = length;
		writeHeaderSector(_sector);
		_header_modified = false;
		_write_pos = startOfData();
		_first_unused_sector = _start_sector + sectorsNeeded(_name_len, length);
		_open_for_write = true;
	}
	virtual void append(byte b)
	{
		if (!_open_for_write)
			return;
		if (_header_modified)
		{
			if (_write_pos > 0)
			{
				fprintf(stderr, "Error: modified header, after append\n");
				return;
			}
			writeHeaderSector(_sector);
			_header_modified = false;
			_write_pos = startOfData();
		}
		if (_write_pos >= SECTOR_SIZE)
		{
			if (_start_sector <= _first_unused_sector)
				_blockDevice.writeBlock(_start_sector, _sector);
			else
				fprintf(stderr, "Error: writing after used sectors at %ld\n", _start_sector);
			_start_sector++;
			_write_pos = 0;
		}
		_sector[_write_pos++] = b;
	}
	virtual void close()
	{
		if (!_open_for_write)
			return;
		if (_header_modified)
		{
			writeHeaderSector(_sector);
			//_header_modified = false;
			//_write_pos = startOfData();
		}
		if (_write_pos > 0)
		{
			for (int i = _write_pos; i < SECTOR_SIZE; i++)
				_sector[i] = 0;
		}
		if (_header_modified || _write_pos > 0)
		{
			_blockDevice.writeBlock(_start_sector, _sector);
			_start_sector++;
		}
		//_header_modified = false;
		//_write_pos =
		_open_for_write = false;
	}

private:
	unsigned long _next_sector;
	bool _valid_previous_sector;
	unsigned long _previous_sector;
	Sector _sector;
	bool _open_for_write;
	bool _header_modified;
	unsigned short _write_pos;
	unsigned long _first_unused_sector;
};

class CachingDirectoryIterator : public AbstractDirectoryIterator
{
	struct Entry : public DirectoryEntry
	{
		Entry(const DirectoryEntry &directoryEntry) : next(0) { *dynamic_cast<DirectoryEntry*>(this) = directoryEntry; }
		~Entry() { delete next; }
		Entry* next;
	};
public:
	CachingDirectoryIterator(AbstractBlockDevice &blockDevice) : AbstractDirectoryIterator(blockDevice), _directoryIterator(blockDevice), _first(0), _previous(0)
	{
		Entry** ref_next =  &_first;
		for (_directoryIterator.init(); _directoryIterator.more(); _directoryIterator.next())
		{
			*ref_next = new Entry(_directoryIterator);
			ref_next = &(*ref_next)->next;
		}
		_append_sector = _directoryIterator.startSector();
	}
	virtual void init()
	{
		_previous = 0;
		_it = _first;
		_more = _it != 0;
		if (_more)
			*dynamic_cast<DirectoryEntry*>(this) = *_it;
		else
			_start_sector = _append_sector;
	}
	void next()
	{
		_previous = _it;
		_it = _it->next;
		_more = _it != 0;
		if (_more)
			*dynamic_cast<DirectoryEntry*>(this) = *_it;
		else
			_start_sector = _append_sector;
	}
	virtual void getSector(Sector &sector)
	{
		_blockDevice.readBlock(_it->startSector(), sector);
	}
	virtual void remove()
	{
		if (_previous != 0)
		{
			_previous->next = _it->next;
			_previous->addAllocated(_it->allocated());
			_it->next = 0;
			delete _it;
			_it = _previous;
			*dynamic_cast<DirectoryEntry*>(this) = *_it;
			_previous = 0;
			_directoryIterator.openModifyHeader(_it->startSector());
			_directoryIterator.setAllocated(_it->allocated());
			_directoryIterator.close();
		}
		else
		{
			_it->clearName();
			_it->setLength(0);
			_directoryIterator.openModifyHeader(_it->startSector());
			_directoryIterator.clearName();
			_directoryIterator.setLength(0);
			_directoryIterator.close();
		}
	}

	virtual void openModifyHeader(unsigned long sector)
	{
		_previous = 0;
		if (_it == 0 || _it->startSector() > sector)
			_it = _first;
		for (; _it != 0 && _it->startSector() <= sector; _it = _it->next)
			if (_it->startSector() == sector)
			{
				_directoryIterator.openModifyHeader(sector);
				*dynamic_cast<DirectoryEntry*>(this) = *_it;
				_open_for_write = true;
				_header_modified = false;
				_write_pos = 0;
				return;
			}
		fprintf(stderr, "Error: openModifiyHeader on non-existing cache header at %ld\n", sector);
	}
	virtual void clearName()
	{
		if (!_open_for_write)
			return;
		_directoryIterator.clearName();
		clearName();
		_it->clearName();
		_header_modified = true;
	}
	virtual void setLength(unsigned long length)
	{
		if (!_open_for_write)
			return;
		_directoryIterator.setLength(length);
		_it->setLength(length);
		_length = length;
		_header_modified = true;
	}
	virtual void setAllocated(unsigned long allocated)
	{
		if (!_open_for_write)
			return;
		_directoryIterator.setAllocated(allocated);
		_it->setAllocated(allocated);
		_allocated = allocated;
		_header_modified = true;
	}
	virtual void openWrite(unsigned long sector, const char*name, unsigned long length, unsigned long allocated)
	{
		_previous = 0;
		//if (_it == 0 || _it->startSector() > sector)
		//	_it = _first;
		Entry **ref = &_first;
		for (; *ref != 0 && (*ref)->startSector() <= startSector(); ref = &(*ref)->next)
			if ((*ref)->startSector() == sector)
			{
				_it = *ref;
				_directoryIterator.openWrite(sector, name, length, allocated);
				*dynamic_cast<DirectoryEntry*>(_it) = _directoryIterator;
				*dynamic_cast<DirectoryEntry*>(this) = *_it;
				_write_pos = startOfData();
				_open_for_write = true;
				return;
			}
		_directoryIterator.openWrite(sector, name, length, allocated);
		Entry *new_entry = new Entry(_directoryIterator);
		new_entry->next = (*ref);
		(*ref) = new_entry;
		_it = new_entry;
		*dynamic_cast<DirectoryEntry*>(this) = *_it;
		_write_pos = startOfData();
		_open_for_write = true;
	}
	virtual void append(byte data)
	{
		if (!_open_for_write)
			return;
		_directoryIterator.append(data);
	}
	virtual void close()
	{
		if (!_open_for_write)
			return;
		_directoryIterator.close();
		_open_for_write = false;
		if (_directoryIterator.startSector() > _append_sector)
			_append_sector = _directoryIterator.startSector();
	}

private:
	RawDirectoryIterator _directoryIterator;
	Entry *_first;
	Entry *_it;
	Entry *_previous;
	bool _open_for_write;
	bool _header_modified;
	unsigned short _write_pos;
	unsigned long _append_sector;
};

void dump_file(FILE *f)
{
	unsigned char ch = fgetc(f);
	int col = 0;
	int line = 0;
	fprintf(stderr, "%3d:", line);
	while (!feof(f))
	{
		if (' ' <= ch && ch < 127)
			fprintf(stderr, " %c", ch);
		else
			fprintf(stderr, "%02x", (unsigned short)ch);
		if (++col == SECTOR_SIZE)
		{
			fprintf(stderr, "\n");
			col = 0;
			line++;
			fprintf(stderr, "%3d:", line);
			//if (line++ > 20)
			//	break;
		}
		ch = fgetc(f);
	}
	fprintf(stderr, "\n");
}

void listFiles(SDFileSystem &sdFileSystem, FILE* fout)
{
	for (sdFileSystem.directoryIterator().init(); sdFileSystem.directoryIterator().more(); sdFileSystem.directoryIterator().next())
	{
		fprintf(fout, "%6ld %6ld %6ld %s\n",
				sdFileSystem.directoryIterator().startSector(),
				sdFileSystem.directoryIterator().allocated(),
				sdFileSystem.directoryIterator().length(),
				sdFileSystem.directoryIterator().name());
	}
}

void writeFile(SDFileSystem &sdFileSystem, const char* name, byte ch, unsigned long length)
{
	fprintf(stderr, "\n---------------------\nwriteFile %s %c %ld\n", name, ch, length);
	byte data[1000];
	for (int i = 0; i < length; i++)
		data[i] = ch;
	sdFileSystem.writeFile(name, data, length);
	fprintf(stderr, "writeFile Done\n\n");
	SDFileSystem::ReadStream readStream(sdFileSystem, name);
	if (!readStream.found())
	{
		fprintf(stderr, "Error: file %s does not exist\n", name);
		return;
	}
	unsigned long pos = 0;
	for(; readStream.more(); readStream.next(), pos++)
	{
		if (readStream.value() != ch)
		{
			fprintf(stderr, "Error: at %d value is %c instead of %c\n", pos, readStream.value(), ch);
			return;
		}
	}
	if (pos != length)
	{
		fprintf(stderr, "Error: %ld != %ld\n", pos, length);
		return;
	}
	fprintf(stderr, "writeFile Verified\n");
}
		
int main(int argc, char *argv[])
{
#ifdef _WIN32	
	int fh = open("Test.sdfs", O_RDWR|O_CREAT);
#else
	int fh = open("Test.sdfs", O_RDWR|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
#endif
	FileBlockDevice fileBlockDevice(fh);
	CachingDirectoryIterator directoryIterator(fileBlockDevice);
	//RawDirectoryIterator directoryIterator(fileBlockDevice);
	SDFileSystem sdFileSystem(directoryIterator);
	
	for (int i = 5; i < 20; i++)
	{
		char name[2];
		name[0] = 'a' + (i % 6);
		name[1] = '\0';
		writeFile(sdFileSystem, name, 'b'+i, i*10);
	}

	for (int i = 0; i < 20; i++)
	{
		char name[2];
		name[0] = 'a' + (i % 6);
		name[1] = '\0';
		writeFile(sdFileSystem, name, 'b'+i, (i%5)*10);
	}
	fprintf(stderr, "\n");
	listFiles(sdFileSystem, stderr);
	fprintf(stderr, "\n");

	close(fh);
	fprintf(stderr, "after close\n");
	
	FILE *f = fopen("Test.sdfs", "rb");
	fprintf(stderr, "after open\n");
	dump_file(f);
	fclose(f);
	
/*
	SDFileSystem::ReadStream readStream(sdFileSystem, "test.txt");
	if (readStream.found())
	{
		fprintf(stdout, "Found, data: ");
		for (; readStream.more(); readStream.next())
		{
			fprintf(stdout, "%c", readStream.value());
		}
		fprintf(stdout, "\n");
	}
	else
		fprintf(stdout, "Not found\n");
*/
	return 0;
}

/****************************** OLD ****************************/

/*
	// Test checksum
#define D16 1 << 16	
	unsigned short best_dd;
	unsigned short best_vv;
	int best = 10000;
	byte d[D16];
	//unsigned short dd = 7;
	for (unsigned short dd = 3; dd <= 15; dd++)
	{
		for (unsigned short vv = 3; vv <= 15; vv++)
	//unsigned short vv = 3;
		{
			for (int i = 0; i < D16; i++)
				d[i] = 0;
			//unsigned short dd = atoi(argv[1]);
			//unsigned short vv = atoi(argv[2]);
			for (unsigned long i = 0; i < D16; i++)
				d[(unsigned short)((unsigned short)i / dd + (unsigned short)i * vv)]++;
			int counts[256];
			for (int i = 0; i < 256; i++)
				counts[i] = 0;
			for (int i = 0; i < D16; i++)
				counts[d[i]]++;
			if (counts[0] < best)
			{
				best = counts[0];
				best_dd = dd;
				best_vv = vv;
			}
			//for (int i = 0; i < 4; i++)
			//	printf("%3d: %d\n", i, counts[i]);
		}
	}
	printf("%d %d %d\n", best, best_dd, best_vv);
*/

