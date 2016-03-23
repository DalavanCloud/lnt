//===-cPerf.cpp - Linux perf.data parsing ---------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a python module that reads perf.data files generated by Linux Perf.
// Writing efficiently-executing Python that reads and manipulates binary files
// is difficult and results in code that looks very un-Python-like.
//
// Also, parsing profiles is on the critical path for result submission and
// often has to run on target devices that may not be as fast as a desktop
// machine (an example is a pandaboard - people still test on Cortex-A9).
//
// This algorithm aims to be faster than 'perf report' or 'perf annotate'.
// In experiments this module is around 6x faster than 'perf annotate' - more
// when we reduce the amount of data imported (see later).
//
// Algorithm
// ---------
//
// We start by mmapping the perf.data file and reading the header, event
// attribute entries and the sample data stream. Every sample is aggregated into
// a set of counters counting every event for every PC location (indexed by mmap
// ID - a data stream can contain samples for the same PC in different binaries
// e.g. if exec() is called). Total counters are also kept for the entire stream
// and per-mmap.
//
// The source for the perf.data format is here: https://lwn.net/Articles/644919/
// and the perf_events manpage.
//
// Once the aggregation is done, we do a single pass through the event data:
//
//   for each mmap:
//       if the mmap's event total is < 1% of the total in all counters,
//         then discard the mmap [1].
//
//       load symbol data by calling "nm" and parsing the result.
//       for all PCs we have events for, in sorted order:
//           find the symbol this PC is part of -> Sym
//           look up all PCs between Sym.Start and Sym.End and emit data
//
// The output of this module (cPerf.importPerf) is a dictionary in (almost)
// ProfileV1 form (see profile.py and profilev1impl.py). The only difference is
// that all counters are absolute.
//
// [1]: Perf will start sampling from the moment the perf wrapper tool is
// invoked, and its samples will continue until the perf wrapper tool exits.
// This means that it will often take one or two samples in intermediate
// binaries like "perf", "bash", or libaries such as libdl. We don't care about
// these, and these binaries and libraries are very large to nm and objdump.
//
// So we have a threshold - if a binary contains < 1% of all samples, don't
// bother importing it.
//
//===----------------------------------------------------------------------===//

#ifndef STANDALONE
#include <Python.h>
#endif
#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <sstream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

#ifndef STANDALONE
#ifdef assert
#undef assert
#endif

// Redefine assert so we can use it without destroying the Python interpreter!
#define assert(expr) Assert((expr), #expr, __FILE__, __LINE__)

#endif

// Remove a uint32_t from a byte stream
uint32_t TakeU32(unsigned char *&Buf) {
  uint32_t X = * (uint32_t*) Buf;
  Buf += sizeof(uint32_t);
  return X;
}

// Remove a uint64_t from a byte stream
uint64_t TakeU64(unsigned char *&Buf) {
  uint64_t X = * (uint64_t*) Buf;
  Buf += sizeof(uint64_t);
  return X;
}

// Forks, execs Cmd under a shell and returns
// a file descriptor reading the command's stdout.
FILE *ForkAndExec(std::string Cmd) {
  int P[2];
  pipe(P);

  FILE *Stream = fdopen(P[0], "r");
  
  if (fork() == 0) {
    dup2(P[1], 1);
    close(P[0]);
    execl("/bin/sh", "sh", "-c", Cmd.c_str(), 0);
  } else {
    close(P[1]);
  }

  return Stream;
}

void Assert(bool Expr, const char *ExprStr, const char *File, int Line) {
  if (Expr)
    return;
  char Str[512];
  sprintf(Str, "%s (%s:%d)", ExprStr, File, Line);
  throw std::logic_error(Str);
}

// Returns true if the ELF file given by filename
// is a shared object (DYN).
bool IsSharedObject(std::string Fname) {
  // We replicate the first part of an ELF header here
  // so as not to rely on <elf.h>.
  struct PartialElfHeader {
    unsigned char e_ident[16];
    uint16_t e_type;
  };
  const int ET_DYN = 3;

  FILE *stream = fopen(Fname.c_str(), "r");
  if (stream == NULL)
    return false;

  PartialElfHeader H;
  auto NumRead = fread(&H, 1, sizeof(H), stream);
  assert(NumRead == sizeof(H));

  fclose(stream);

  return H.e_type == ET_DYN;
}

//===----------------------------------------------------------------------===//
// Perf structures. Taken from https://lwn.net/Articles/644919/
//===----------------------------------------------------------------------===//

#define PERF_RECORD_MMAP 1
#define PERF_RECORD_SAMPLE 9
#define PERF_RECORD_MMAP2 10

#define PERF_SAMPLE_IP    (1U << 0)
#define PERF_SAMPLE_TID   (1U << 1)
#define PERF_SAMPLE_TIME  (1U << 2)
#define PERF_SAMPLE_ADDR  (1U << 3)
#define PERF_SAMPLE_ID    (1U << 6)
#define PERF_SAMPLE_CPU   (1U << 7)
#define PERF_SAMPLE_PERIOD (1U << 8)
#define PERF_SAMPLE_STREAM_ID (1U << 9)
#define PERF_SAMPLE_IDENTIFIER (1U << 16)

struct perf_file_section {
  uint64_t offset; /* offset from start of file */
  uint64_t size;   /* size of the section */
};

struct perf_header {
  char magic[8];      /* PERFILE2 */
  uint64_t size;      /* size of the header */
  uint64_t attr_size; /* size of an attribute in attrs */
  struct perf_file_section attrs;
  struct perf_file_section data;
  struct perf_file_section event_types;
  uint64_t flags;
  uint64_t flags1[3];
};

struct perf_event_header {
  uint32_t type;
  uint16_t misc;
  uint16_t size;
};

struct perf_event_sample {
  struct perf_event_header header;

  uint64_t sample_id;
  uint64_t ip;
  uint32_t pid, tid;
  uint64_t time;
  uint64_t id;
  uint64_t period;
};

struct perf_event_mmap {
  struct perf_event_header header;

  uint32_t pid, tid;
  uint64_t start, extent, pgoff;
  char filename[1];
};

struct perf_event_mmap2 {
  struct perf_event_header header;

  uint32_t pid, tid;
  uint64_t start, extent, pgoff;
  uint32_t maj, min;
  uint64_t ino, ino_generation;
  uint32_t prot, flags;
  char filename[1];
};

struct perf_trace_event_type {
  uint64_t event_id;
  char str[64];
};

//===----------------------------------------------------------------------===//
// Readers for nm and objdump output
//===----------------------------------------------------------------------===//

struct Map {
  uint64_t Start, End;
  const char *Filename;
};

struct Symbol {
  uint64_t Start;
  uint64_t End;
  std::string Name;

  bool operator<(const Symbol &Other) const { return Start < Other.Start; }
  bool operator==(const Symbol &Other) const {
    return Start == Other.Start && End == Other.End && Name == Other.Name;
  }
};

class NmOutput : public std::vector<Symbol> {
public:
  std::string Nm;

  NmOutput(std::string Nm) : Nm(Nm) {}

  void fetchSymbols(Map *M, bool Dynamic) {
    std::string D = "-D";
    if (!Dynamic)
      // Don't fetch the dynamic symbols - instead fetch static ones.
      D = "";
    std::string Cmd = Nm + " " + D + " -S --defined-only " + std::string(M->Filename) +
                      " 2>/dev/null";
    auto Stream = ForkAndExec(Cmd);

    char *Line = nullptr;
    size_t LineLen = 0;
    while (true) {
      ssize_t Len = getline(&Line, &LineLen, Stream);
      if (Len == -1)
        break;

      char One[32], Two[32], Three[32], Four[512];
      if (sscanf(Line, "%s %s %s %s", One, Two, Three, Four) < 4)
        continue;

      char *EndPtr = NULL;
      uint64_t Start = strtoull(One, &EndPtr, 16);
      if (EndPtr == One)
        continue;
      uint64_t Extent = strtoull(Two, &EndPtr, 16);
      if (EndPtr == Two)
        continue;
      if (strlen(Three) != 1)
        continue;
      switch (Three[0]) {
      default:
        continue;
      case 'T':
      case 't': // Text section
      case 'V':
      case 'v': // Weak object
      case 'W':
      case 'w': // Weak object (not tagged as such)
        break;
      }
      std::string S(Four);
      if (S.back() == '\n')
        S.pop_back();
      push_back({Start, Start + Extent, S});
    }
    if (Line)
      free(Line);

    fclose(Stream);
    wait(NULL);
  }

  void reset(Map *M) {
    clear();
    // Fetch both dynamic and static symbols, sort and unique them.
    fetchSymbols(M, true);
    fetchSymbols(M, false);
    
    std::sort(begin(), end());
    auto NewEnd = std::unique(begin(), end());
    erase(NewEnd, end());
  }
};

class ObjdumpOutput {
public:
  std::string Objdump;
  FILE *Stream;
  char *ThisText;
  uint64_t ThisAddress;
  uint64_t EndAddress;
  char *Line;
  size_t LineLen;

  ObjdumpOutput(std::string Objdump)
    : Objdump(Objdump), Stream(nullptr), Line(NULL), LineLen(0) {}
  ~ObjdumpOutput() {
    if (Stream) {
      fclose(Stream);
      wait(NULL);
    }
    if (Line)
      free(Line);
  }

  void reset(Map *M, uint64_t Start, uint64_t Stop) {
    ThisAddress = 0;
    if (Stream) {
      fclose(Stream);
      wait(NULL);
    }

    char buf1[32], buf2[32];
    sprintf(buf1, "%#llx", Start);
    sprintf(buf2, "%#llx", Stop + 4);

    std::string Cmd = Objdump + " -d --no-show-raw-insn --start-address=" +
                      std::string(buf1) + " --stop-address=" +
                      std::string(buf2) + " " + std::string(M->Filename) +
                      " 2>/dev/null";
    Stream = ForkAndExec(Cmd);

    EndAddress = Stop;
  };

  std::string getText() { return ThisText; }

  uint64_t next() {
    getLine();
    return ThisAddress;
  }

  void getLine() {
    while (true) {
      ssize_t Len = getline(&Line, &LineLen, Stream);
      if (Len == -1) {
        ThisAddress = EndAddress;
        return;
      }
      char *TokBuf;
      char *One = strtok_r(Line, ":", &TokBuf);
      char *Two = strtok_r(NULL, "\n", &TokBuf);
      if (!One || !Two)
        continue;
      char *EndPtr = NULL;
      uint64_t Address = strtoull(One, &EndPtr, 16);
      if (EndPtr != &One[strlen(One)])
        continue;

      ThisAddress = Address;
      ThisText = Two;
      break;
    }
  }
};

//===----------------------------------------------------------------------===//
// PerfReader
//===----------------------------------------------------------------------===//

class PerfReader {
public:
  PerfReader(const std::string &Filename, std::string Nm,
             std::string Objdump);
  ~PerfReader();

  void readHeader();
  void readAttrs();
  void readDataStream();
  unsigned char *readEvent(unsigned char *);
  perf_event_sample parseEvent(unsigned char *Buf, uint64_t Layout);
  void emitLine(uint64_t PC, std::map<const char *, uint64_t> *Counters,
                const std::string &Text);
  void emitFunctionStart(std::string &Name);
  void emitFunctionEnd(std::string &Name,
                       std::map<const char *, uint64_t> &Counters);
  void emitTopLevelCounters();
  void emitMaps();
  void emitSymbol(
      Symbol &Sym, Map &M,
      std::map<uint64_t, std::map<const char *, uint64_t>>::iterator &Event,
      uint64_t Adjust);
  PyObject *complete();

private:
  unsigned char *Buffer;
  size_t BufferLen;

  perf_header *Header;
  std::map<uint64_t, const char *> EventIDs;
  std::map<uint64_t, uint64_t> EventLayouts;
  std::map<size_t, std::map<uint64_t, std::map<const char *, uint64_t>>> Events;
  std::map<const char *, uint64_t> TotalEvents;
  std::map<uint64_t, std::map<const char *, uint64_t>> TotalEventsPerMap;
  std::vector<Map> Maps;
  std::map<uint64_t, size_t> CurrentMaps;

  PyObject *Functions, *TopLevelCounters;
  std::vector<PyObject*> Lines;
  
  std::string Nm, Objdump;
};

PerfReader::PerfReader(const std::string &Filename,
                       std::string Nm, std::string Objdump)
    : Nm(Nm), Objdump(Objdump) {
  int fd = open(Filename.c_str(), O_RDONLY);
  assert(fd > 0);

  struct stat sb;
  assert(fstat(fd, &sb) == 0);
  BufferLen = (size_t)sb.st_size;

  Buffer = (unsigned char *)mmap(NULL, BufferLen, PROT_READ, MAP_SHARED, fd, 0);
  assert(Buffer != MAP_FAILED);
}

PerfReader::~PerfReader() { munmap(Buffer, BufferLen); }

void PerfReader::readHeader() {
  Header = (perf_header *)&Buffer[0];

  assert(!strncmp(Header->magic, "PERFILE2", 8));
}

void PerfReader::readDataStream() {
  unsigned char *Buf = &Buffer[Header->data.offset];
  unsigned char *End = Buf + Header->data.size;
  while (Buf < End)
    Buf = readEvent(Buf);
}

void PerfReader::readAttrs() {
  const int HEADER_EVENT_DESC = 12;
  perf_file_section *P =
      (perf_file_section *)&Buffer[Header->data.offset + Header->data.size];
  for (int I = 0; I < HEADER_EVENT_DESC; ++I)
    if (Header->flags & (1U << I))
      ++P;

  assert(Header->flags & (1U << HEADER_EVENT_DESC));

  unsigned char *Buf = &Buffer[P->offset];
  uint32_t NumEvents = TakeU32(Buf);
  uint32_t AttrSize = TakeU32(Buf);
  for (unsigned I = 0; I < NumEvents; ++I) {
    // Parse out the "config" bitmask for the struct layout.
    auto *Buf2 = Buf;
    (void)TakeU32(Buf2); (void)TakeU32(Buf2); (void)TakeU64(Buf2); (void)TakeU64(Buf2);
    auto Layout = TakeU64(Buf2);
    
    Buf += AttrSize;
    uint32_t NumIDs = TakeU32(Buf);

    uint32_t StrLen = TakeU32(Buf);
    const char *Str = (const char *)Buf;
    Buf += StrLen;

    // Weirdness of perf: if there is only one event descriptor, that
    // event descriptor can be referred to by ANY id!
    if (NumEvents == 1 && NumIDs == 0) {
      EventIDs[0] = Str;
      EventLayouts[0] = Layout;
    }
    
    for (unsigned J = 0; J < NumIDs; ++J) {
      auto L = TakeU64(Buf);
      EventIDs[L] = Str;
      EventLayouts[L] = Layout;
    }
  }
}

unsigned char *PerfReader::readEvent(unsigned char *Buf) {
  perf_event_sample *E = (perf_event_sample *)Buf;
  
  if (E->header.type == PERF_RECORD_MMAP) {
    perf_event_mmap *E = (perf_event_mmap *)Buf;
    auto MapID = Maps.size();
    Maps.push_back({E->start, E->start + E->extent, E->filename});

    // Clear out the current range if applicable.
    auto L = CurrentMaps.lower_bound(E->start);
    auto H = CurrentMaps.upper_bound(E->start + E->extent);
    CurrentMaps.erase(L, H);

    CurrentMaps.insert({E->start, MapID});
  }
  if (E->header.type == PERF_RECORD_MMAP2) {
    perf_event_mmap2 *E = (perf_event_mmap2 *)Buf;
    auto MapID = Maps.size();
    Maps.push_back({E->start, E->start + E->extent, E->filename});

    // Clear out the current range if applicable.
    auto L = CurrentMaps.lower_bound(E->start);
    auto H = CurrentMaps.upper_bound(E->start + E->extent);
    CurrentMaps.erase(L, H);

    CurrentMaps.insert({E->start, MapID});
  }

  if (E->header.type != PERF_RECORD_SAMPLE)
    return &Buf[E->header.size];

  
  auto NewE = parseEvent(((unsigned char*)E) + sizeof(perf_event_header),
                         EventLayouts.begin()->second);
  auto EventID = NewE.id;
  auto PC = NewE.ip;
  auto MapID = std::prev(CurrentMaps.upper_bound(PC))->second;

  assert(EventIDs.count(EventID));
  Events[MapID][PC][EventIDs[EventID]] += NewE.period;

  TotalEvents[EventIDs[EventID]] += NewE.period;
  TotalEventsPerMap[MapID][EventIDs[EventID]] += NewE.period;

  return &Buf[E->header.size];
}

perf_event_sample PerfReader::parseEvent(unsigned char *Buf, uint64_t Layout) {
  perf_event_sample E;
  memset((char*)&E, 0, sizeof(E));

  assert(Layout & PERF_SAMPLE_IP);
  assert(Layout & PERF_SAMPLE_PERIOD);

  if (Layout & PERF_SAMPLE_IDENTIFIER)
    E.id = TakeU64(Buf);
  if (Layout & PERF_SAMPLE_IP)
    E.ip = TakeU64(Buf);
  if (Layout & PERF_SAMPLE_TID) {
    E.pid = TakeU32(Buf);
    E.tid = TakeU32(Buf);
  }
  if (Layout & PERF_SAMPLE_TIME)
    E.time = TakeU64(Buf);
  if (Layout & PERF_SAMPLE_ADDR)
    (void) TakeU64(Buf);
  if (Layout & PERF_SAMPLE_ID)
    E.id = TakeU64(Buf);
  if (Layout & PERF_SAMPLE_STREAM_ID)
    (void) TakeU64(Buf);
  if (Layout & PERF_SAMPLE_CPU)
    (void) TakeU64(Buf);
  if (Layout & PERF_SAMPLE_PERIOD)
    E.period = TakeU64(Buf);
  
  return E;
}

void PerfReader::emitFunctionStart(std::string &Name) {
  Lines.clear();
}

void PerfReader::emitFunctionEnd(std::string &Name,
                                 std::map<const char *, uint64_t> &Counters) {
  auto *CounterDict = PyDict_New();
  for (auto &KV : Counters)
    PyDict_SetItemString(CounterDict, KV.first,
                         PyLong_FromUnsignedLongLong((unsigned long long)KV.second));

  auto *LinesList = PyList_New(Lines.size());
  unsigned Idx = 0;
  for (auto *I : Lines)
    PyList_SetItem(LinesList, Idx++, I);
  
  auto *FnDict = PyDict_New();
  PyDict_SetItemString(FnDict, "counters", CounterDict);
  PyDict_SetItemString(FnDict, "data", LinesList);

  PyDict_SetItemString(Functions, Name.c_str(), FnDict);
}

void PerfReader::emitLine(uint64_t PC,
                          std::map<const char *, uint64_t> *Counters,
                          const std::string &Text) {
  auto *CounterDict = PyDict_New();
  if (Counters)
    for (auto &KV : *Counters)
      PyDict_SetItemString(CounterDict, KV.first,
                           PyLong_FromUnsignedLongLong((unsigned long long)KV.second));
  
  auto *Line = Py_BuildValue("[NKs]",
                             CounterDict,
                             (unsigned long long) PC,
                             (char*) Text.c_str());
  Lines.push_back(Line);
}

void PerfReader::emitTopLevelCounters() {
  TopLevelCounters = PyDict_New();
  for (auto &KV : TotalEvents)
    PyDict_SetItemString(TopLevelCounters, KV.first,
                         PyLong_FromUnsignedLongLong((unsigned long long)KV.second));

  Functions = PyDict_New();
}

void PerfReader::emitMaps() {
  for (auto &KV : Events) {
    auto MapID = KV.first;
    auto &MapEvents = KV.second;

    if (MapEvents.empty())
      continue;

    if (MapID > Maps.size()) {
      // Something went badly wrong. Try and recover.
      continue;
    }

    // Are there enough events here to bother with?
    bool AllUnderThreshold = true;
    for (auto &I : TotalEventsPerMap[MapID]) {
      auto Total = TotalEvents[I.first];
      // If a map contains more than 1% of some event, bother with it.
      if ((float)I.second / (float)Total > 0.01f) {
        AllUnderThreshold = false;
        break;
      }
    }
    if (AllUnderThreshold)
      continue;

    // EXEC ELF objects aren't relocated. DYN ones are,
    // so if it's a DYN object adjust by subtracting the
    // map base.
    bool IsSO = IsSharedObject(Maps[MapID].Filename);
    uint64_t Adjust = IsSO ? Maps[MapID].Start : 0;

    NmOutput Syms(Nm);
    Syms.reset(&Maps[MapID]);
    auto Sym = Syms.begin();

    auto Event = MapEvents.begin();
    while (Event != MapEvents.end() && Sym != Syms.end()) {
      auto PC = Event->first - Adjust;
      if (PC < Sym->Start) {
        ++Event;
        continue;
      }
      while (Sym != Syms.end() && PC >= Sym->End)
        ++Sym;
      if (Sym == Syms.end() || Sym->Start > PC) {
        ++Event;
        continue;
      }

      emitSymbol(*Sym++, Maps[MapID], Event, Adjust);
    }
  }
}

void PerfReader::emitSymbol(
    Symbol &Sym, Map &M,
    std::map<uint64_t, std::map<const char *, uint64_t>>::iterator &Event,
    uint64_t Adjust) {
  ObjdumpOutput Dump(Objdump);
  Dump.reset(&M, Sym.Start, Sym.End);
  Dump.next();

  std::map<const char *, uint64_t> SymEvents;

  emitFunctionStart(Sym.Name);
  for (uint64_t I = Sym.Start; I < Sym.End; I = Dump.next()) {
    auto PC = Event->first - Adjust;

    auto Text = Dump.getText();
    if (PC == I) {
      emitLine(I, &Event->second, Text);

      for (auto &KV : Event->second)
        SymEvents[KV.first] += KV.second;

      ++Event;
    } else {
      emitLine(I, nullptr, Text);
    }
  }
  emitFunctionEnd(Sym.Name, SymEvents);
}

PyObject *PerfReader::complete() {
  auto *Obj = PyDict_New();
  PyDict_SetItemString(Obj, "counters", TopLevelCounters);
  PyDict_SetItemString(Obj, "functions", Functions);
  return Obj;
}

#ifndef STANDALONE
static PyObject *cPerf_importPerf(PyObject *self, PyObject *args) {
  const char *Fname;
  const char *Nm = "nm";
  const char *Objdump = "objdump";
  if (!PyArg_ParseTuple(args, "s|ss", &Fname, &Nm, &Objdump))
    return NULL;

  try {
    PerfReader P(Fname, Nm, Objdump);
    P.readHeader();
    P.readAttrs();
    P.readDataStream();
    P.emitTopLevelCounters();
    P.emitMaps();
    return P.complete();
  } catch (std::logic_error &E) {
    PyErr_SetString(PyExc_AssertionError, E.what());
    return NULL;
  } catch (std::runtime_error &E) {
    PyErr_SetString(PyExc_RuntimeError, E.what());
    return NULL;
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Unknown error");
    return NULL;
  }
}

static PyMethodDef cPerfMethods[] = {{"importPerf", cPerf_importPerf,
                                      METH_VARARGS,
                                      "Import perf.data from a filename"},
                                     {NULL, NULL, 0, NULL}};

PyMODINIT_FUNC initcPerf(void) { (void)Py_InitModule("cPerf", cPerfMethods); }

#else // STANDALONE

int main(int argc, char **argv) {
  PerfReader P(argv[1], std::cout);
  P.readHeader();
  P.readAttrs();
  P.readDataStream();
  P.emitTopLevelCounters();
  P.emitMaps();
  P.complete();
}

#endif
