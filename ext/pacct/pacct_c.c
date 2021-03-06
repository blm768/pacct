#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/acct.h>
#include <sys/types.h>

#include "ruby.h"

static char const* validFileModes[] = {
  "rb",
  "wb",
  "r+b",
  "w+b",
};

static VALUE mPacct;
static VALUE cLog;
static VALUE cEntry;

//Classes from Ruby
static VALUE cTime;
static VALUE cNoMemoryError;
static VALUE cSystemCallError;

//Identifiers
static ID id_at;
static ID id_new;
static ID id_to_i;

//To do: where is a better place to put these?
static VALUE known_users_by_name = Qnil;
static VALUE known_groups_by_name = Qnil;
static VALUE known_users_by_id = Qnil;
static VALUE known_groups_by_id = Qnil;

//System parameters
static int pageSize;
static long ticksPerSecond;

//Converts a comp_t to a ulong
static unsigned long comp_t_to_ulong(comp_t c) {
  return (unsigned long)(c & 0x1fff) << (((c >> 13) & 0x7) * 3);
}

//Converts a long to a comp_t
//To consider: make sure the value is positive?
//To consider: more unit testing?
static comp_t ulong_to_comp_t(unsigned long l) {
  size_t bits = 0;
  unsigned long l2 = l;
  // Find the number of bits necessary to represent l
  while(l2) {
    ++bits;
    l2 >>= 1;
  }
  // If the value is small enough to fit in the mantissa without an exponent,
  // just return it.
  if(bits <= 13) {
    return (l & 0x1fff);
  }

  // TODO: try to remember how this logic works. ;)
  size_t rem_bits;
  bits -= 13;
  rem_bits = bits % 3;
  if(rem_bits) {
    bits += (3 - rem_bits);
  }
  if(bits >= 24) {
    rb_raise(rb_eRangeError, "Exponent overflow in ulong_to_comp_t: Value %lu is too large.", l);
  }
  //To consider: remove '&'?
  return ((l >> bits) & 0x1fff) | ((bits / 3) << 13);
}

//Checks the result of a call, raising an error if it fails
//To do: handle non-integer values in the rb_raise format string?
#define CHECK_CALL(expr, expected_result) \
  { \
    typeof(expr) expected = (expected_result); \
    typeof(expr) result; \
    errno = 0; \
    result = (expr); \
    if(result != expected) { \
      if(errno) { \
        char buf[512]; \
        VALUE err; \
        snprintf(buf, sizeof(buf), "%s(%u)", __FILE__, __LINE__); \
        err = rb_funcall(cSystemCallError, id_new, 2, rb_str_new2(buf), INT2NUM(errno)); \
        rb_exc_raise(err); \
      } else { \
        rb_raise(rb_eRuntimeError, #expr ": result %i expected, not %i - %s(%u)", expected, result, __FILE__, __LINE__); \
      } \
    } \
  } \

#define ENSURE_ALLOCATED(ptr) if(!ptr) rb_raise(cNoMemoryError, "Out of memory");

typedef struct {
  FILE* file;
  char* filename;
  long num_entries;
} PacctLog;

static void pacct_log_free(void* p) {
  PacctLog* log = (PacctLog*) p;
  if(log->file) {
    fclose(log->file);
    log->file = NULL;
  }
  free(log->filename);
  free(p);
}

/*
 *call-seq:
 *  new(filename)
 *
 *Creates a new Pacct::Log using the given accounting file
 */
static VALUE pacct_log_new(int argc, VALUE* argv, VALUE class) {
  VALUE log;
  VALUE init_args[2];
  PacctLog* ptr;

  init_args[1] = Qnil;
  rb_scan_args(argc, argv, "11", init_args, init_args + 1);

  log = Data_Make_Struct(class, PacctLog, 0, pacct_log_free, ptr);

  ptr->file = NULL;
  ptr->num_entries = 0;

  rb_obj_call_init(log, 2, init_args);
  return log;
}

static VALUE pacct_log_init(VALUE self, VALUE filename, VALUE mode) {
  PacctLog* log;
  FILE* acct;
  long length;
  char* c_filename = StringValueCStr(filename);
  size_t c_filename_len;
  const char* c_mode = "rb";

  if(mode != Qnil) {
    int isValidMode = 0;
    size_t i;
    c_mode = StringValueCStr(mode);
    for(i = 0; i < sizeof(validFileModes) / sizeof(char*); ++i) {
      if(strcmp(c_mode, validFileModes[i]) == 0) {
        isValidMode = 1;
        break;
      }
    }
    if(!isValidMode) {
      rb_raise(rb_eArgError, "Invalid mode for Pacct::File: '%s'", c_mode);
    }
  }

  acct = fopen(c_filename, c_mode);
  if(!acct) {
    rb_raise(rb_eIOError, "Unable to open file '%s'", c_filename);
  }

  Data_Get_Struct(self, PacctLog, log);

  log->file = acct;
  c_filename_len = strlen(c_filename);
  log->filename = malloc(c_filename_len + 1);
  ENSURE_ALLOCATED(log->filename);
  strncpy(log->filename, c_filename, c_filename_len);
  log->filename[c_filename_len] = '\0';

  CHECK_CALL(fseek(acct, 0, SEEK_END), 0);
  length = ftell(acct);
  rewind(acct);

  if(length % sizeof(struct acct_v3) != 0) {
    rb_raise(rb_eIOError, "Accounting file '%s' appears to be the wrong size.", c_filename);
  }

  log->num_entries = length / sizeof(struct acct_v3);

  return self;
}

static void pacct_log_check_closed(PacctLog* log) {
  if(!log->file) {
      rb_raise(rb_eRuntimeError, "The file '%s' has already been closed.", log->filename);
    }
}

/*
 *Closes the log file
 */
static VALUE pacct_log_close(VALUE self) {
  PacctLog* log;

  Data_Get_Struct(self, PacctLog, log);

  if(log->file) {
    fclose(log->file);
    log->file = NULL;
  }

  return Qnil;
}

static VALUE pacct_entry_new(PacctLog* log) {
  struct acct_v3* ptr;
  VALUE entry = Data_Make_Struct(cEntry, struct acct_v3, 0, free, ptr);
  if(log) {
    size_t entries_read;
    // TODO: just let fread() catch this case?
    pacct_log_check_closed(log);
    entries_read = fread(ptr, sizeof(struct acct_v3), 1, log->file);
    if(entries_read != 1) {
      // TODO: pass errno in the exception.
      rb_raise(rb_eIOError, "Unable to read record from accounting file '%s'", log->filename);
    }
  } else {
    memset(ptr, 0, sizeof(struct acct_v3));
  }

  return entry;
}

//This is the version of pacct_entry_new that is actually exposed to Ruby.
static VALUE ruby_pacct_entry_new(VALUE self) {
  return pacct_entry_new(NULL);
}

/*
 *call-seq:
 *  each_entry([start]) {|entry, index| ...}
 *
 *Yields each entry in the file to the given block
 *
 *If start is given, iteration starts at the entry with that index.
 */
static VALUE each_entry(int argc, VALUE* argv, VALUE self) {
  PacctLog* log;
  VALUE start_value;
  long start = 0;
  int i = 0;

  rb_scan_args(argc, argv, "01", &start_value);
  if(argc && start_value != Qnil) {
    start = NUM2UINT(start_value);
  }

  Data_Get_Struct(self, PacctLog, log);

  pacct_log_check_closed(log);

  if(start > log->num_entries) {
    rb_raise(rb_eRangeError, "Index %li is out of range", start);
  }

  CHECK_CALL(fseek(log->file, start * sizeof(struct acct_v3), SEEK_SET), 0);

  for(i = start; i < log->num_entries; ++i) {
    VALUE entry = pacct_entry_new(log);
    rb_yield(entry);
  }

  return Qnil;
}

/*
 *Returns the last entry in the file
 */
static VALUE last_entry(VALUE self) {
  PacctLog* log;
  long pos;
  VALUE entry;

  Data_Get_Struct(self, PacctLog, log);

  pacct_log_check_closed(log);

  if(log->num_entries == 0) {
    return Qnil;
  }

  pos = ftell(log->file);
  CHECK_CALL(fseek(log->file, -sizeof(struct acct_v3), SEEK_END), 0);

  entry = pacct_entry_new(log);

  CHECK_CALL(fseek(log->file, pos, SEEK_SET), 0);

  return entry;
}

/*
 *Returns the number of entries in the file
 */
static VALUE get_num_entries(VALUE self) {
  PacctLog* log;

  Data_Get_Struct(self, PacctLog, log);

  return INT2NUM(log->num_entries);
}

/*
 *call-seq:
 *  write_entry(entry)
 *
 * Appends the given entry to the file
 */
static VALUE write_entry(VALUE self, VALUE entry) {
  //To do consider: verification?
  PacctLog* log;
  long pos;
  struct acct_v3* acct;

  Data_Get_Struct(self, PacctLog, log);
  pacct_log_check_closed(log);
  Data_Get_Struct(entry, struct acct_v3, acct);

  pos = ftell(log->file);
  CHECK_CALL(fseek(log->file, 0, SEEK_END), 0);

  if(fwrite(acct, sizeof(struct acct_v3), 1, log->file) != 1) {
    rb_raise(rb_eIOError, "Unable to write to accounting file '%s'", log->filename);
  }

  ++(log->num_entries);

  CHECK_CALL(fseek(log->file, pos, SEEK_SET), 0);

  return Qnil;
}

//Methods of Pacct::Entry
/*
 *Returns the process ID
 */
static VALUE get_process_id(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(data->ac_pid);
}

/*
 *Sets the process ID
 */
static VALUE set_process_id(VALUE self, VALUE pid) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_pid = NUM2UINT(pid);

  return Qnil;
}

/*
 *Returns the ID of the user who executed the command
 */
static VALUE get_user_id(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(data->ac_uid);
}

/*
 *Returns the name of the user who executed the command
 */
static VALUE get_user_name(VALUE self) {
  struct acct_v3* data;
  struct passwd* pw_data;
  VALUE id, name;
  Data_Get_Struct(self, struct acct_v3, data);

  //If there's a cached user name, return it.
  id = UINT2NUM(data->ac_uid);
  name = rb_hash_aref(known_users_by_id, id);
  if(name != Qnil) {
    return name;
  }

  //Otherwise, get the user name from the OS.
  errno = 0;
  pw_data = getpwuid(data->ac_uid);
  if(!pw_data) {
    char buf[512];
    VALUE err;
    int e = errno;
    snprintf(buf, 512, "Unable to obtain user name for ID %u", data->ac_uid);
    if(e == 0) {
        e = ENODATA;
    }
    err = rb_funcall(cSystemCallError, id_new, 2, rb_str_new2(buf), INT2NUM(e));
    rb_exc_raise(err);
  }

  //Cache the user name.
  name = rb_str_new2(pw_data->pw_name);
  rb_hash_aset(known_users_by_id, id, name);

  return name;
}

/*
 *Sets the name of the user who executed the command
 */
static VALUE set_user_name(VALUE self, VALUE name) {
  struct acct_v3* data;
  struct passwd* pw_data;
  char* c_name = StringValueCStr(name);
  VALUE id;
  Data_Get_Struct(self, struct acct_v3, data);

  id = rb_hash_aref(known_users_by_name, name);
  if(id != Qnil) {
    data->ac_uid = NUM2UINT(id);
    return Qnil;
  }

  errno = 0;
  pw_data = getpwnam(c_name);
  if(!pw_data) {
    char buf[512];
    VALUE err;
    int e = errno;
    snprintf(buf, 512, "Unable to obtain user ID for name '%s'", c_name);
    if(e == 0) {
        e = ENODATA;
    }
    err = rb_funcall(cSystemCallError, id_new, 2, rb_str_new2(buf), INT2NUM(e));
    rb_exc_raise(err);
  }

  id = UINT2NUM(pw_data->pw_uid);
  rb_hash_aset(known_users_by_name, name, id);

  data->ac_uid = pw_data->pw_uid;

  return Qnil;
}

/*
 *Returns the group ID of the user who executed the command
 */
static VALUE get_group_id(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(data->ac_gid);
}

/*
 *Returns the group name of the user who executed the command
 */
static VALUE get_group_name(VALUE self) {
  struct acct_v3* data;
  struct group* group_data;
  VALUE name, id;
  Data_Get_Struct(self, struct acct_v3, data);

  id = UINT2NUM(data->ac_gid);
  name = rb_hash_aref(known_groups_by_id, id);
  if(name != Qnil) {
    return name;
  }

  errno = 0;
  group_data = getgrgid(data->ac_gid);
  if(!group_data) {
    char buf[512];
    VALUE err;
    int e = errno;
    snprintf(buf, 512, "Unable to obtain group name for ID %u", data->ac_gid);
    if(e == 0) {
      e = ENODATA;
    }
    err = rb_funcall(cSystemCallError, id_new, 2, rb_str_new2(buf), INT2NUM(e));
    rb_exc_raise(err);
  }

  name = rb_str_new2(group_data->gr_name);
  rb_hash_aset(known_groups_by_id, id, name);

  return name;
}

/*
 *Sets the group name of the user who executed the command
 */
static VALUE set_group_name(VALUE self, VALUE name) {
  struct acct_v3* data;
  struct group* group_data;
  VALUE id;
  char* c_name = StringValueCStr(name);
  Data_Get_Struct(self, struct acct_v3, data);

  id = rb_hash_aref(known_groups_by_name, name);
  if(id != Qnil) {
    data->ac_gid = NUM2UINT(id);
    return Qnil;
  }

  errno = 0;
  group_data = getgrnam(c_name);
  if(!group_data) {
    char buf[512];
    VALUE err;
    int e = errno;
    snprintf(buf, 512, "Unable to obtain group ID for name '%s'", c_name);
    if(e == 0) {
      e = ENODATA;
    }
    err = rb_funcall(cSystemCallError, id_new, 2, rb_str_new2(buf), INT2NUM(e));
    rb_exc_raise(err);
  }

  id = UINT2NUM(group_data->gr_gid);
  rb_hash_aset(known_groups_by_name, name, id);

  data->ac_gid = group_data->gr_gid;

  return Qnil;
}

/*
 *Returns the task's total user CPU time in seconds
 */
static VALUE get_user_time(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(comp_t_to_ulong(data->ac_utime) / ticksPerSecond);
}

/*
 *Sets the task's total user CPU time
 */
static VALUE set_user_time(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_utime = ulong_to_comp_t(NUM2ULONG(value) * ticksPerSecond);

  return Qnil;
}

/*
 *Returns the task's total system CPU time in seconds
 */
static VALUE get_system_time(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(comp_t_to_ulong(data->ac_stime) / ticksPerSecond);
}

/*
 *Sets the task's total system CPU time
 */
static VALUE set_system_time(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_stime = ulong_to_comp_t(NUM2ULONG(value) * ticksPerSecond);

  return Qnil;
}

/*
 *Returns the task's total CPU time in seconds
 */
static VALUE get_cpu_time(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM((comp_t_to_ulong(data->ac_utime) + comp_t_to_ulong(data->ac_stime)) / ticksPerSecond);
}

/*
 *Returns the task's total wall time in seconds
 */
static VALUE get_wall_time(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return rb_float_new(data->ac_etime);
}

/*
 *Sets the task's total wall time
 */
static VALUE set_wall_time(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_etime = NUM2DBL(value);

  return Qnil;
}

/*
 *Returns the task's start time
 */
static VALUE get_start_time(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return rb_funcall(cTime, id_at, 1, INT2NUM(data->ac_btime));
}

/*
 *Sets the task's start time
 */
static VALUE set_start_time(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_btime = NUM2UINT(rb_funcall(value, id_to_i, 0));

  return Qnil;
}

/*
 *Returns the task's average memory usage in kilobytes
 */
static VALUE get_average_mem_usage(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  //Why divided by page size?
  return INT2NUM(comp_t_to_ulong(data->ac_mem) * 1024 / pageSize);
}

/*
 *Sets the task's average memory usage in kilobytes
 */
static VALUE set_average_mem_usage(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_mem = ulong_to_comp_t(NUM2ULONG(value) * pageSize / 1024);

  return Qnil;
}

/*
 *Returns the first 15 characters of the command name
 */
static VALUE get_command_name(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return rb_str_new2(data->ac_comm);
}

/*
 *Sets the first 15 characters of the command name
 */
static VALUE set_command_name(VALUE self, VALUE name) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  strncpy(data->ac_comm, StringValueCStr(name), ACCT_COMM - 1);
  data->ac_comm[ACCT_COMM - 1] = '\0';

  return Qnil;
}

/*
 *Returns the command's exit code
 */
static VALUE get_exit_code(VALUE self) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  return INT2NUM(data->ac_exitcode);
}

/*
 *Sets the command's exit code
 */
static VALUE set_exit_code(VALUE self, VALUE value) {
  struct acct_v3* data;
  Data_Get_Struct(self, struct acct_v3, data);

  data->ac_exitcode = NUM2UINT(value);

  return Qnil;
}

//Unit testing code

static VALUE test_check_call_macro(VALUE self, VALUE test) {
  int i = NUM2INT(test);
  switch(i) {
    case 0:
      CHECK_CALL(0, 0);
      break;
    case 1:
      CHECK_CALL(1, 0);
      break;
    case 2:
      CHECK_CALL(errno = 0, 1);
    case 3:
      CHECK_CALL(errno = ERANGE, 0);
    default:
      rb_raise(rb_eRangeError, "Unknown test code %i", i);
  }
  return Qnil;
}

static VALUE test_read_failure(VALUE self) {
  PacctLog log;
  //VALUE entry = pacct_entry_new(NULL);
  const char* filename = "/dev/null";
  log.num_entries = 0;
  log.filename = malloc(strlen(filename) + 1);
  ENSURE_ALLOCATED(log.filename);
  strcpy(log.filename, filename);
  log.file = fopen(log.filename, "r");

  pacct_entry_new(&log);
  return Qnil;
}

static VALUE test_write_failure(VALUE self) {
  PacctLog* ptr;
  VALUE log = Data_Make_Struct(cLog, PacctLog, 0, pacct_log_free, ptr);
  VALUE entry = pacct_entry_new(NULL);
  const char* filename = "spec/pacct_spec.rb";
  ptr->num_entries = 0;
  ptr->filename = malloc(strlen(filename) + 1);
  ENSURE_ALLOCATED(ptr->filename);
  strcpy(ptr->filename, filename);
  ptr->file = fopen(ptr->filename, "r");

  write_entry(log, entry);
  return Qnil;
}

static VALUE test_ulong_to_comp_t(VALUE self, VALUE val) {
  unsigned long l = (unsigned long)NUM2ULONG(val);
  comp_t result = ulong_to_comp_t(l);
  return INT2NUM(result);
}

static VALUE test_comp_t_to_ulong(VALUE self, VALUE val) {
  comp_t c = (comp_t)NUM2UINT(val);
  unsigned long result = comp_t_to_ulong(c);
  return ULONG2NUM(result);
}

void Init_pacct_c() {
  VALUE mRSpec;

  // Get system parameters
  // NOTE: if the process accounting files were not created on the machine that
  // executes this code, the values may be off. In practice, that shouldn't be
  // a major issue on x86_64 systems with modern Linux kernels.
  pageSize = getpagesize();
  ticksPerSecond = sysconf(_SC_CLK_TCK);

  //Get Ruby objects.
  cTime = rb_const_get(rb_cObject, rb_intern("Time"));
  cSystemCallError = rb_const_get(rb_cObject, rb_intern("SystemCallError"));
  cNoMemoryError = rb_const_get(rb_cObject, rb_intern("NoMemoryError"));

  id_at = rb_intern("at");
  id_new = rb_intern("new");
  id_to_i = rb_intern("to_i");

  //To consider: is there any place that these can be unregistered?
  rb_gc_register_address(&known_users_by_name);
  rb_gc_register_address(&known_groups_by_name);
  rb_gc_register_address(&known_users_by_id);
  rb_gc_register_address(&known_groups_by_id);

  known_users_by_name = rb_hash_new();
  known_groups_by_name = rb_hash_new();
  known_users_by_id = rb_hash_new();
  known_groups_by_id = rb_hash_new();

  //Define Ruby modules/objects/methods.
  mPacct = rb_define_module("Pacct");
  /*
   *Represents an accounting file in acct(5) format
   */
  cLog = rb_define_class_under(mPacct, "Log", rb_cObject);
  /*
   *Document-class: Pacct::Entry
   *
   *Represents an entry in a Pacct::File
   */
  cEntry = rb_define_class_under(mPacct, "Entry", rb_cObject);
  rb_define_singleton_method(cLog, "new", pacct_log_new, -1);
  rb_define_method(cLog, "initialize", pacct_log_init, 2);
  rb_define_method(cLog, "each_entry", each_entry, -1);
  rb_define_method(cLog, "last_entry", last_entry, 0);
  rb_define_method(cLog, "num_entries", get_num_entries, 0);
  rb_define_method(cLog, "write_entry", write_entry, 1);
  rb_define_method(cLog, "close", pacct_log_close, 0);

  rb_define_singleton_method(cEntry, "new", ruby_pacct_entry_new, 0);
  rb_define_method(cEntry, "process_id", get_process_id, 0);
  rb_define_method(cEntry, "process_id=", set_process_id, 1);
  rb_define_method(cEntry, "user_id", get_user_id, 0);
  rb_define_method(cEntry, "user_name", get_user_name, 0);
  rb_define_method(cEntry, "user_name=", set_user_name, 1);
  rb_define_method(cEntry, "group_id", get_group_id, 0);
  rb_define_method(cEntry, "group_name", get_group_name, 0);
  rb_define_method(cEntry, "group_name=", set_group_name, 1);
  rb_define_method(cEntry, "user_time", get_user_time, 0);
  rb_define_method(cEntry, "user_time=", set_user_time, 1);
  rb_define_method(cEntry, "system_time", get_system_time, 0);
  rb_define_method(cEntry, "system_time=", set_system_time, 1);
  rb_define_method(cEntry, "cpu_time", get_cpu_time, 0);
  rb_define_method(cEntry, "wall_time", get_wall_time, 0);
  rb_define_method(cEntry, "wall_time=", set_wall_time, 1);
  rb_define_method(cEntry, "start_time", get_start_time, 0);
  rb_define_method(cEntry, "start_time=", set_start_time, 1);
  rb_define_method(cEntry, "memory", get_average_mem_usage, 0);
  rb_define_method(cEntry, "memory=", set_average_mem_usage, 1);
  rb_define_method(cEntry, "exit_code", get_exit_code, 0);
  rb_define_method(cEntry, "exit_code=", set_exit_code, 1);
  rb_define_method(cEntry, "command_name", get_command_name, 0);
  rb_define_method(cEntry, "command_name=", set_command_name, 1);

  //To consider: support other testing frameworks?

  mRSpec = rb_const_defined(rb_cObject, rb_intern("RSpec"));
  if(mRSpec == Qtrue) {
    /*
     *Document-module: Pacct::Test
     *
     *Contains unit-testing methods
     *
     *Only defined when RSpec is loaded
     */
    VALUE mTest = rb_define_module_under(mPacct, "Test");
    rb_define_module_function(mTest, "check_call", test_check_call_macro, 1);
    rb_define_module_function(mTest, "write_failure", test_write_failure, 0);
    rb_define_module_function(mTest, "read_failure", test_read_failure, 0);
    rb_define_module_function(mTest, "comp_t_to_ulong", test_comp_t_to_ulong, 1);
    //To do: create unit test.
    rb_define_module_function(mTest, "ulong_to_comp_t", test_ulong_to_comp_t, 1);
  }
}
