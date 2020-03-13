/////////////////////////////////////////////////////////////////////////
// $Id: main.cc 13241 2017-05-28 08:13:06Z vruppert $
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2001-2017  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

#include "bochs.h"
#include "bxversion.h"
#include "param_names.h"
#include "cpu/cpu.h"
#include "iodev/iodev.h"

// TETRANE INCLUDE
#include <experimental/optional>
#include <boost/program_options.hpp>

#include "tetrane/bochs_replayer/util/log.h"
#include "tetrane/bochs_replayer/replayer/replayer.h"
#include "tetrane/bochs_replayer/tracer/tracer.h"
#include "tetrane/bochs_replayer/memhist_tracer/memhist_tracer.h"
#include "tetrane/bochs_replayer/icount/icount.h"
// END TETRANE INCLUDE

extern "C" {
#include <libgen.h>
#include <signal.h>
}

int  bx_init_main(int argc, char *argv[], const char* bochsrc_filename);
void bx_init_hardware(void);
void bx_init_options(void);
void bx_init_bx_dbg(void);

static const char *divider = "========================================================================";

bx_startup_flags_t bx_startup_flags;
bx_bool bx_user_quit;
Bit8u bx_cpu_count;
#if BX_SUPPORT_APIC
Bit32u apic_id_mask; // determinted by XAPIC option
bx_bool simulate_xapic;
#endif

/* typedefs */

#define LOG_THIS genlog->

bx_pc_system_c bx_pc_system;

bx_debug_t bx_dbg;

typedef BX_CPU_C *BX_CPU_C_PTR;

#if BX_SUPPORT_SMP
// multiprocessor simulation, we need an array of cpus
BOCHSAPI BX_CPU_C_PTR *bx_cpu_array = NULL;
#else
// single processor simulation, so there's one of everything
BOCHSAPI BX_CPU_C bx_cpu;
#endif

BOCHSAPI BX_MEM_C bx_mem;

// TETRANE DEFINITION
namespace reven {
namespace util {

std::uint8_t verbose_level;

}
}

reven::replayer::Replayer replayer;
std::experimental::optional<reven::tracer::Tracer> tracer;
std::experimental::optional<reven::memhist_tracer::MemhistTracer> memhist_tracer;
reven::icount::ICount tick_counter;
// END TETRANE DEFINITION

#if BX_DEBUGGER
void print_tree(bx_param_c *node, int level, bx_bool xml)
{
  int i;
  char tmpstr[BX_PATHNAME_LEN];

  for (i=0; i<level; i++)
    dbg_printf("  ");
  if (node == NULL) {
      dbg_printf("NULL pointer\n");
      return;
  }

  if (xml)
    dbg_printf("<%s>", node->get_name());
  else
    dbg_printf("%s = ", node->get_name());

  switch (node->get_type()) {
    case BXT_PARAM_NUM:
    case BXT_PARAM_BOOL:
    case BXT_PARAM_ENUM:
    case BXT_PARAM_STRING:
      node->dump_param(tmpstr, BX_PATHNAME_LEN, 1);
      dbg_printf("%s", tmpstr);
      break;
    case BXT_LIST:
      {
        if (!xml) dbg_printf("{");
        dbg_printf("\n");
        bx_list_c *list = (bx_list_c*)node;
        for (i=0; i < list->get_size(); i++) {
          print_tree(list->get(i), level+1, xml);
        }
        for (i=0; i<level; i++)
          dbg_printf("  ");
        if (!xml) dbg_printf("}");
        break;
      }
    case BXT_PARAM_DATA:
      dbg_printf("'binary data size=%d'", ((bx_shadow_data_c*)node)->get_size());
      break;
    default:
      dbg_printf("(unknown parameter type)");
  }

  if (xml) dbg_printf("</%s>", node->get_name());
  dbg_printf("\n");
}
#endif

#if BX_ENABLE_STATISTICS
void print_statistics_tree(bx_param_c *node, int level)
{
  for (int i=0; i<level; i++)
    fprintf(stderr, "  ");
  if (node == NULL) {
      fprintf(stderr, "NULL pointer\n");
      return;
  }
  switch (node->get_type()) {
    case BXT_PARAM_NUM:
      {
        bx_param_num_c* param = (bx_param_num_c*) node;
        fprintf(stderr, "%s = " FMT_LL "d\n", node->get_name(), param->get64());
        param->set(0); // clear the statistic
      }
      break;
    case BXT_PARAM_BOOL:
      BX_PANIC(("boolean statistics are not supported !"));
      break;
    case BXT_PARAM_ENUM:
      BX_PANIC(("enum statistics are not supported !"));
      break;
    case BXT_PARAM_STRING:
      BX_PANIC(("string statistics are not supported !"));
      break;
    case BXT_LIST:
      {
        bx_list_c *list = (bx_list_c*)node;
        if (list->get_size() > 0) {
          fprintf(stderr, "%s = \n", node->get_name());
          for (int i=0; i < list->get_size(); i++) {
            print_statistics_tree(list->get(i), level+1);
          }
        }
        break;
      }
    case BXT_PARAM_DATA:
      BX_PANIC(("binary data statistics are not supported !"));
      break;
    default:
      BX_PANIC(("%s (unknown parameter type)\n", node->get_name()));
      break;
  }
}
#endif

BxEvent *
tetrane_notify_callback(void *unused, BxEvent *event)
{
  event->retcode = -1;
  switch (event->type)
  {
    case BX_SYNC_EVT_TICK:
      event->retcode = 0;
      return event;
    case BX_SYNC_EVT_ASK_PARAM:
      event->retcode = event->u.param.param->text_ask();
      return event;
    case BX_ASYNC_EVT_REFRESH:
    case BX_ASYNC_EVT_DBG_MSG:
    case BX_ASYNC_EVT_LOG_MSG:
      // The text mode interface does not use these events, so just ignore
      // them.
      return event;
    default:
      fprintf(stderr, "TETRANE: notify callback called with event type %04x\n", event->type);
      return event;
  }
  assert(0); // switch statement should return
}

namespace {

struct counter { int count = 0; };
void validate(boost::any& v, std::vector<std::string> const& xs, counter*, long) {
  if (v.empty()) v = counter{1};
  else ++boost::any_cast<counter&>(v).count;
}

}

int bxmain(void)
{
  counter verbose;

  std::string bxshare_path;
  std::string bochsrc_filename;

  std::string core_file;
  std::string analyze_directory;

  std::string trace_directory("./");
  std::string memhist_file("./memhist.sqlite");
  std::uint64_t max_icount = std::numeric_limits<std::uint64_t>::max();

  namespace progopts = boost::program_options;

  progopts::options_description desc("Allowed options");

  desc.add_options()
            ("help", "Produce help message.")
            ("bxshare", progopts::value<std::string>(&bxshare_path), "The bxshare path (could also be overwritten by the environment variable BXSHARE")
            ("bochsrc", progopts::value<std::string>(&bochsrc_filename), "The bochsrc file")
            ("core", progopts::value<std::string>(&core_file)->required(), "The input core file")
            ("analyze", progopts::value<std::string>(&analyze_directory)->required(), "The analyze directory")
            ("trace", progopts::value<std::string>(&trace_directory)->implicit_value(trace_directory), "Enable the trace output")
            ("memhist", progopts::value<std::string>(&memhist_file)->implicit_value(memhist_file), "Enable the memory history output")
            ("max-icount", progopts::value<std::uint64_t>(&max_icount), "Maximum number of instructions replayed")

            ("fail-on-desync", "Return an error code of 1 in case of desync")

            ("verbose,v", progopts::value(&verbose)->zero_tokens(), "Verbosity level")
            ("version", "Display the version")
  ;

  progopts::variables_map vars;

  try {
    progopts::store(progopts::command_line_parser(bx_startup_flags.argc, bx_startup_flags.argv)
      .options(desc)
      .allow_unregistered()
      .run(), vars);

    if (vars.count("help")) {
      std::cout << desc << std::endl;
      return 0;
    }

    if (vars.count("version")) {
      std::cout << "Bochs - Replayer version " << GIT_VERSION << std::endl;
      std::cout << REL_STRING << std::endl;
#ifdef __DATE__
#ifdef __TIME__
      std::cout << "Compiled on " << __DATE__ << " at " << __TIME__ << std::endl;
#else
      std::cout << "Compiled on " << __DATE__ << std::endl;
#endif
#endif
      return 0;
    }

    progopts::notify(vars);
  }
  catch (const std::exception& err) {
    std::cerr << "Error: " << err.what() << std::endl;
    return 1;
  }

  reven::util::verbose_level = verbose.count;

  if (!vars.count("bxshare")) {
    const char* bxshare_env = getenv("BXSHARE");
    if (bxshare_env != NULL) {
      bxshare_path = bxshare_env;
      std::cout << "BXSHARE is set to '" << bxshare_env << "'" << std::endl;
    } else {
      std::cout << "BXSHARE not set. using compile time default '" << BX_SHARE_PATH << "'" << std::endl;
      bxshare_path = BX_SHARE_PATH;
      setenv("BXSHARE", BX_SHARE_PATH, 1);
    }
  } else {
    std::cout << "BXSHARE is set to '" << bxshare_path << "'" << std::endl;
  }

  if (!vars.count("bochsrc")) {
    bochsrc_filename = bxshare_path + "/bochsrc";
  }

  std::cout << "Bochsrc filename is set to '" << bochsrc_filename << "'" << std::endl;

  if (vars.count("trace")) {
    reven::tracer::initialize_register_maps();
    tracer.emplace(trace_directory);

    std::cout << "Build trace in " << trace_directory << std::endl;
  }

  if (vars.count("memhist")) {
    memhist_tracer.emplace(memhist_file);

    std::cout << "Build memhist in " << memhist_file << std::endl;
  }

  if (vars.count("max-icount")) {
    tick_counter = reven::icount::ICount(max_icount);
  }

  if (!replayer.load(core_file, analyze_directory)) {
    return 1;
  }

  bx_init_siminterface();   // create the SIM object

  BX_INSTR_INIT_ENV();

  SIM->set_quit_context(NULL);
  SIM->set_notify_callback(tetrane_notify_callback, NULL);

  if (bx_init_main(bx_startup_flags.argc, bx_startup_flags.argv, bochsrc_filename.c_str()) < 0) {
    BX_INSTR_EXIT_ENV();
    return 0;
  }

  try {
    // We can't let the user choose the memory size, it must match the one of the original VM during the record

    const auto ram_size = replayer.get_memory_size() / (1024 * 1024);
    std::cerr << "TETRANE: Info: Forcing to use memory of size : " << std::dec << ram_size << " MB" << std::endl;

    if (ram_size > 2048) {
      LOG_FATAL_ERROR("The RAM of the VMs can't be more than 2048MB");
      return 1;
    }

    SIM->get_param_num(BXPN_MEM_SIZE)->set(ram_size);
    SIM->get_param_num(BXPN_HOST_MEM_SIZE)->set(ram_size);

    BX_INSTR_EXIT_ENV();

    SIM->begin_simulation(bx_startup_flags.argc, bx_startup_flags.argv);
  } catch(const std::exception& e) {
    LOG_FATAL_ERROR("Exception: " << e.what())
    return 1;
  }

  if (vars.count("fail-on-desync") && replayer.get_desync()) {
    return 2;
  }

  return SIM->get_exit_code();
}

// normal main function, presently in for all cases except for
// wxWidgets under win32.
int CDECL main(int argc, char *argv[])
{
  bx_startup_flags.argc = argc;
  bx_startup_flags.argv = argv;

  return bxmain();
}

void print_usage(void)
{
  fprintf(stderr,
    "Usage: bochs [flags] [bochsrc options]\n\n"
    "  -n               no configuration file\n"
    "  -f configfile    specify configuration file\n"
    "  -q               quick start (skip configuration interface)\n"
    "  -benchmark N     run bochs in benchmark mode for N millions of emulated ticks\n"
#if BX_ENABLE_STATISTICS
    "  -dumpstats N     dump bochs stats every N millions of emulated ticks\n"
#endif
    "  -r path          restore the Bochs state from path\n"
    "  -log filename    specify Bochs log file name\n"
#if BX_DEBUGGER
    "  -rc filename     execute debugger commands stored in file\n"
    "  -dbglog filename specify Bochs internal debugger log file name\n"
#endif
#ifdef WIN32
    "  -noconsole       disable console window\n"
#endif
    "  --help           display this help and exit\n"
    "  --help features  display available features / devices and exit\n"
#if BX_CPU_LEVEL > 4
    "  --help cpu       display supported CPU models and exit\n"
#endif
    "\nFor information on Bochs configuration file arguments, see the\n"
#if (!defined(WIN32)) && !BX_WITH_MACOS
    "bochsrc section in the user documentation or the man page of bochsrc.\n");
#else
    "bochsrc section in the user documentation.\n");
#endif
}

int bx_init_main(int argc, char *argv[], const char* bochsrc_filename)
{
  // To deal with initialization order problems inherent in C++, use the macros
  // SAFE_GET_IOFUNC and SAFE_GET_GENLOG to retrieve "io" and "genlog" in all
  // constructors or functions called by constructors.  The macros test for
  // NULL and create the object if necessary, then return it.  Ensure that io
  // and genlog get created, by making one reference to each macro right here.
  // All other code can reference io and genlog directly.  Because these
  // objects are required for logging, and logging is so fundamental to
  // knowing what the program is doing, they are never free()d.
  SAFE_GET_IOFUNC();  // never freed
  SAFE_GET_GENLOG();  // never freed

  // initalization must be done early because some destructors expect
  // the bochs config options to exist by the time they are called.
  bx_init_bx_dbg();
  bx_init_options();

  //SIM->get_param_enum(BXPN_CPU_MODEL)->set_by_name("corei7_sandy_bridge_2600k"); // TETRANE: Force the CPU

  // interpret the args that start with -, like -q, -f, etc.
  int arg = 1, load_rcfile=1;
  while (arg < argc) {
    // parse next arg
    if (!strcmp("--help", argv[arg]) || !strncmp("-h", argv[arg], 2)
       ) {
      if ((arg+1) < argc) {
        if (!strcmp("features", argv[arg+1])) {
          fprintf(stderr, "Supported features:\n\n");
#if BX_SUPPORT_CLGD54XX
          fprintf(stderr, "cirrus\n");
#endif
#if BX_SUPPORT_VOODOO
          fprintf(stderr, "voodoo\n");
#endif
#if BX_SUPPORT_PCI
          fprintf(stderr, "pci\n");
#endif
#if BX_SUPPORT_PCIDEV
          fprintf(stderr, "pcidev\n");
#endif
#if BX_SUPPORT_NE2K
          fprintf(stderr, "ne2k\n");
#endif
#if BX_SUPPORT_PCIPNIC
          fprintf(stderr, "pcipnic\n");
#endif
#if BX_SUPPORT_E1000
          fprintf(stderr, "e1000\n");
#endif
#if BX_SUPPORT_SB16
          fprintf(stderr, "sb16\n");
#endif
#if BX_SUPPORT_ES1370
          fprintf(stderr, "es1370\n");
#endif
#if BX_SUPPORT_USB_OHCI
          fprintf(stderr, "usb_ohci\n");
#endif
#if BX_SUPPORT_USB_UHCI
          fprintf(stderr, "usb_uhci\n");
#endif
#if BX_SUPPORT_USB_EHCI
          fprintf(stderr, "usb_ehci\n");
#endif
#if BX_SUPPORT_USB_XHCI
          fprintf(stderr, "usb_xhci\n");
#endif
#if BX_GDBSTUB
          fprintf(stderr, "gdbstub\n");
#endif
          fprintf(stderr, "\n");
          arg++;
        }
#if BX_CPU_LEVEL > 4
        else if (!strcmp("cpu", argv[arg+1])) {
          int i = 0;
          fprintf(stderr, "Supported CPU models:\n\n");
          do {
            fprintf(stderr, "%s\n", SIM->get_param_enum(BXPN_CPU_MODEL)->get_choice(i));
          } while (i++ < SIM->get_param_enum(BXPN_CPU_MODEL)->get_max());
          fprintf(stderr, "\n");
          arg++;
        }
#endif
      } else {
        print_usage();
      }
      SIM->quit_sim(0);
    }
    else {
      // the arg did not start with -, so stop interpreting flags
      break;
    }
    arg++;
  }
#if BX_PLUGINS
  // set a default plugin path, in case the user did not specify one
#if BX_WITH_CARBON
  // if there is no stdin, then we must create our own LTDL_LIBRARY_PATH.
  // also if there is no LTDL_LIBRARY_PATH, but we have a bundle since we're here
  // This is here so that it is available whenever --with-carbon is defined but
  // the above code might be skipped, as in --with-sdl --with-carbon
  if(!isatty(STDIN_FILENO) || !getenv("LTDL_LIBRARY_PATH"))
  {
    CFBundleRef mainBundle;
    CFURLRef libDir;
    char libDirPath[MAXPATHLEN];
    if(!isatty(STDIN_FILENO))
    {
      // there is no stdin/stdout so disable the text-based config interface.
      SIM->get_param_enum(BXPN_BOCHS_START)->set(BX_QUICK_START);
    }
    BX_INFO(("fixing default lib location ..."));
    // locate the lib directory within the application bundle.
    // our libs have been placed in bochs.app/Contents/(current platform aka MacOS)/lib
    // This isn't quite right, but they are platform specific and we haven't put
    // our plugins into true frameworks and bundles either
    mainBundle = CFBundleGetMainBundle();
    BX_ASSERT(mainBundle != NULL);
    libDir = CFBundleCopyAuxiliaryExecutableURL(mainBundle, CFSTR("lib"));
    BX_ASSERT(libDir != NULL);
    // translate this to a unix style full path
    if(!CFURLGetFileSystemRepresentation(libDir, true, (UInt8 *)libDirPath, MAXPATHLEN))
    {
      BX_PANIC(("Unable to work out ltdl library path within bochs bundle! (Most likely path too long!)"));
      return -1;
    }
    setenv("LTDL_LIBRARY_PATH", libDirPath, 1);
    BX_INFO(("now my LTDL_LIBRARY_PATH is %s", getenv("LTDL_LIBRARY_PATH")));
    CFRelease(libDir);
  }
#elif BX_HAVE_GETENV && BX_HAVE_SETENV
  if (getenv("LTDL_LIBRARY_PATH") != NULL) {
    BX_INFO(("LTDL_LIBRARY_PATH is set to '%s'", getenv("LTDL_LIBRARY_PATH")));
  } else {
    BX_INFO(("LTDL_LIBRARY_PATH not set. using compile time default '%s'",
        BX_PLUGIN_PATH));
    setenv("LTDL_LIBRARY_PATH", BX_PLUGIN_PATH, 1);
  }
#endif
#endif  /* if BX_PLUGINS */

  // initialize plugin system. This must happen before we attempt to
  // load any modules.
  plugin_startup();

  // load pre-defined optional plugins before parsing configuration
  SIM->opt_plugin_ctrl("*", 1);
  SIM->init_save_restore();
  SIM->init_statistics();

  if (!bx_read_configuration(bochsrc_filename))
    return 0;

  return 1;
}

bx_bool load_and_init_display_lib(void)
{
  if (bx_gui != NULL) {
    // bx_gui has already been filled in.  This happens when you start
    // the simulation for the second time.
    // Also, if you load wxWidgets as the configuration interface.  Its
    // plugin_init will install wxWidgets as the bx_gui.
    return 1;
  }
  BX_ASSERT(bx_gui == NULL);
  bx_param_enum_c *ci_param = SIM->get_param_enum(BXPN_SEL_CONFIG_INTERFACE);
  const char *ci_name = ci_param->get_selected();
  bx_param_enum_c *gui_param = SIM->get_param_enum(BXPN_SEL_DISPLAY_LIBRARY);
  const char *gui_name = gui_param->get_selected();
  PLUG_load_gui_plugin(gui_name);

  return (bx_gui != NULL);
}

extern bx_bool bx_dbg_read_pmode_descriptor(Bit16u sel, bx_descriptor_t *descriptor);

void tetrane_simulation() {
  // Reset and launch the execution of the CPU 0
  replayer.reset(0);

  if (tracer)
    tracer->init(0, replayer);

  replayer.execute(0);

  if (tracer)
    tracer->end();

  if (memhist_tracer)
    memhist_tracer->end();
}

int bx_begin_simulation(int argc, char *argv[])
{
  bx_user_quit = 0;

  // make sure all optional plugins have been loaded
  SIM->opt_plugin_ctrl("*", 1);

  // deal with gui selection
  if (!load_and_init_display_lib()) {
    BX_PANIC(("no gui module was loaded"));
    return 0;
  }

  bx_cpu_count = 1;
  BX_ASSERT(bx_cpu_count > 0);

  bx_init_hardware();

  SIM->set_init_done(1);

  tetrane_simulation();

  BX_INFO(("cpu loop quit, shutting down simulator"));
  bx_atexit();
  return(0);
}

void bx_stop_simulation(void)
{
  // in wxWidgets, the whole simulator is running in a separate thread.
  // our only job is to end the thread as soon as possible, NOT to shut
  // down the whole application with an exit.
  BX_CPU(0)->async_event = 1;
  bx_pc_system.kill_bochs_request = 1;
  // the cpu loop will exit very soon after this condition is set.
}

void bx_sr_after_restore_state(void)
{
#if BX_SUPPORT_SMP == 0
  BX_CPU(0)->after_restore_state();
#else
  for (unsigned i=0; i<BX_SMP_PROCESSORS; i++) {
    BX_CPU(i)->after_restore_state();
  }
#endif
  DEV_after_restore_state();
}

void bx_set_log_actions_by_device(bx_bool panic_flag)
{
  int id, l, m, val;
  bx_list_c *loglev, *level;
  bx_param_num_c *action;

  loglev = (bx_list_c*) SIM->get_param("general.logfn");
  for (l = 0; l < loglev->get_size(); l++) {
    level = (bx_list_c*) loglev->get(l);
    for (m = 0; m < level->get_size(); m++) {
      action = (bx_param_num_c*) level->get(m);
      id = SIM->get_logfn_id(action->get_name());
      val = action->get();
      if (id < 0) {
        if (panic_flag) {
          BX_PANIC(("unknown log function module '%s'", action->get_name()));
        }
      } else if (val >= 0) {
        SIM->set_log_action(id, l, val);
        // mark as 'done'
        action->set(-1);
      }
    }
  }
}

void bx_init_hardware()
{
  int i;
  char pname[16];
  bx_list_c *base;

  // all configuration has been read, now initialize everything.

  bx_pc_system.initialize(SIM->get_param_num(BXPN_IPS)->get());

  if (SIM->get_param_string(BXPN_LOG_FILENAME)->getptr()[0]!='-') {
    BX_INFO(("using log file %s", SIM->get_param_string(BXPN_LOG_FILENAME)->getptr()));
    io->init_log(SIM->get_param_string(BXPN_LOG_FILENAME)->getptr());
  }

  io->set_log_prefix(SIM->get_param_string(BXPN_LOG_PREFIX)->getptr());

  // Output to the log file the cpu and device settings
  // This will by handy for bug reports
  BX_INFO(("Bochs Replayer"));
  BX_INFO(("  %s", REL_STRING));
#ifdef __DATE__
#ifdef __TIME__
  BX_INFO(("Compiled on %s at %s", __DATE__, __TIME__));
#else
  BX_INFO(("Compiled on %s", __DATE__));
#endif
#endif
  BX_INFO(("System configuration"));
  BX_INFO(("  processors: %d (cores=%u, HT threads=%u)", BX_SMP_PROCESSORS,
    SIM->get_param_num(BXPN_CPU_NCORES)->get(), SIM->get_param_num(BXPN_CPU_NTHREADS)->get()));
  BX_INFO(("  A20 line support: %s", BX_SUPPORT_A20?"yes":"no"));
#if BX_CONFIGURE_MSRS
  const char *msrs_file = SIM->get_param_string(BXPN_CONFIGURABLE_MSRS_PATH)->getptr();
  if ((strlen(msrs_file) > 0) && strcmp(msrs_file, "none"))
    BX_INFO(("  load configurable MSRs from file \"%s\"", msrs_file));
#endif
  BX_INFO(("IPS is set to %d", (Bit32u) SIM->get_param_num(BXPN_IPS)->get()));
  BX_INFO(("CPU configuration"));
#if BX_SUPPORT_SMP
  BX_INFO(("  SMP support: yes, quantum=%d", SIM->get_param_num(BXPN_SMP_QUANTUM)->get()));
#else
  BX_INFO(("  SMP support: no"));
#endif

  unsigned cpu_model = SIM->get_param_enum(BXPN_CPU_MODEL)->get();
  if (! cpu_model) {
#if BX_CPU_LEVEL >= 5
    unsigned cpu_level = SIM->get_param_num(BXPN_CPUID_LEVEL)->get();
    BX_INFO(("  level: %d", cpu_level));
    BX_INFO(("  APIC support: %s", SIM->get_param_enum(BXPN_CPUID_APIC)->get_selected()));
#else
    BX_INFO(("  level: %d", BX_CPU_LEVEL));
    BX_INFO(("  APIC support: no"));
#endif
    BX_INFO(("  FPU support: %s", BX_SUPPORT_FPU?"yes":"no"));
#if BX_CPU_LEVEL >= 5
    bx_bool mmx_enabled = SIM->get_param_bool(BXPN_CPUID_MMX)->get();
    BX_INFO(("  MMX support: %s", mmx_enabled?"yes":"no"));
    BX_INFO(("  3dnow! support: %s", BX_SUPPORT_3DNOW?"yes":"no"));
#endif
#if BX_CPU_LEVEL >= 6
    bx_bool sep_enabled = SIM->get_param_bool(BXPN_CPUID_SEP)->get();
    BX_INFO(("  SEP support: %s", sep_enabled?"yes":"no"));
    BX_INFO(("  SIMD support: %s", SIM->get_param_enum(BXPN_CPUID_SIMD)->get_selected()));
    bx_bool xsave_enabled = SIM->get_param_bool(BXPN_CPUID_XSAVE)->get();
    bx_bool xsaveopt_enabled = SIM->get_param_bool(BXPN_CPUID_XSAVEOPT)->get();
    BX_INFO(("  XSAVE support: %s %s",
      xsave_enabled?"xsave":"no", xsaveopt_enabled?"xsaveopt":""));
    bx_bool aes_enabled = SIM->get_param_bool(BXPN_CPUID_AES)->get();
    BX_INFO(("  AES support: %s", aes_enabled?"yes":"no"));
    bx_bool sha_enabled = SIM->get_param_bool(BXPN_CPUID_SHA)->get();
    BX_INFO(("  SHA support: %s", sha_enabled?"yes":"no"));
    bx_bool movbe_enabled = SIM->get_param_bool(BXPN_CPUID_MOVBE)->get();
    BX_INFO(("  MOVBE support: %s", movbe_enabled?"yes":"no"));
    bx_bool adx_enabled = SIM->get_param_bool(BXPN_CPUID_ADX)->get();
    BX_INFO(("  ADX support: %s", adx_enabled?"yes":"no"));
#if BX_SUPPORT_X86_64
    bx_bool x86_64_enabled = SIM->get_param_bool(BXPN_CPUID_X86_64)->get();
    BX_INFO(("  x86-64 support: %s", x86_64_enabled?"yes":"no"));
    bx_bool xlarge_enabled = SIM->get_param_bool(BXPN_CPUID_1G_PAGES)->get();
    BX_INFO(("  1G paging support: %s", xlarge_enabled?"yes":"no"));
#else
    BX_INFO(("  x86-64 support: no"));
#endif
#if BX_SUPPORT_MONITOR_MWAIT
    bx_bool mwait_enabled = SIM->get_param_bool(BXPN_CPUID_MWAIT)->get();
    BX_INFO(("  MWAIT support: %s", mwait_enabled?"yes":"no"));
#endif
#if BX_SUPPORT_VMX
    unsigned vmx_enabled = SIM->get_param_num(BXPN_CPUID_VMX)->get();
    if (vmx_enabled) {
      BX_INFO(("  VMX support: %d", vmx_enabled));
    }
    else {
      BX_INFO(("  VMX support: no"));
    }
#endif
#if BX_SUPPORT_SVM
    bx_bool svm_enabled = SIM->get_param_bool(BXPN_CPUID_SVM)->get();
    BX_INFO(("  SVM support: %s", svm_enabled?"yes":"no"));
#endif
#endif // BX_CPU_LEVEL >= 6
  }
  else {
    BX_INFO(("  Using pre-defined CPU configuration: %s",
      SIM->get_param_enum(BXPN_CPU_MODEL)->get_selected()));
  }

  BX_INFO(("Optimization configuration"));
  BX_INFO(("  RepeatSpeedups support: %s", BX_SUPPORT_REPEAT_SPEEDUPS?"yes":"no"));
  BX_INFO(("  Fast function calls: %s", BX_FAST_FUNC_CALL?"yes":"no"));
  BX_INFO(("  Handlers Chaining speedups: %s", BX_SUPPORT_HANDLERS_CHAINING_SPEEDUPS?"yes":"no"));
  BX_INFO(("Devices configuration"));
  BX_INFO(("  PCI support: %s", BX_SUPPORT_PCI?"i440FX i430FX":"no"));
#if BX_SUPPORT_NE2K || BX_SUPPORT_E1000
  BX_INFO(("  Networking support:%s%s",
           BX_SUPPORT_NE2K?" NE2000":"", BX_SUPPORT_E1000?" E1000":""));
#else
  BX_INFO(("  Networking: no"));
#endif
#if BX_SUPPORT_SB16 || BX_SUPPORT_ES1370
  BX_INFO(("  Sound support:%s%s",
           BX_SUPPORT_SB16?" SB16":"", BX_SUPPORT_ES1370?" ES1370":""));
#else
  BX_INFO(("  Sound support: no"));
#endif
#if BX_SUPPORT_PCIUSB
  BX_INFO(("  USB support:%s%s%s%s",
           BX_SUPPORT_USB_UHCI?" UHCI":"", BX_SUPPORT_USB_OHCI?" OHCI":"",
           BX_SUPPORT_USB_EHCI?" EHCI":"", BX_SUPPORT_USB_XHCI?" xHCI":""));
#else
  BX_INFO(("  USB support: no"));
#endif
  BX_INFO(("  VGA extension support: vbe%s%s",
           BX_SUPPORT_CLGD54XX?" cirrus":"", BX_SUPPORT_VOODOO?" voodoo":""));

  // Check if there is a romimage
  if (SIM->get_param_string(BXPN_ROM_PATH)->isempty()) {
    BX_ERROR(("No romimage to load. Is your bochsrc file loaded/valid ?"));
  }

  // set one shot timer for benchmark mode if needed, the timer will fire
  // once and kill Bochs simulation after predefined amount of emulated
  // ticks
  int benchmark_mode = SIM->get_param_num(BXPN_BOCHS_BENCHMARK)->get();
  if (benchmark_mode) {
    BX_INFO(("Bochs benchmark mode is ON (~%d millions of ticks)", benchmark_mode));
    bx_pc_system.register_timer_ticks(&bx_pc_system, bx_pc_system_c::benchmarkTimer,
        (Bit64u) benchmark_mode * 1000000, 0 /* one shot */, 1, "benchmark.timer");
  }

#if BX_ENABLE_STATISTICS
  // set periodic timer for dumping statistics collected during Bochs run
  int dumpstats = SIM->get_param_num(BXPN_DUMP_STATS)->get();
  if (dumpstats) {
    BX_INFO(("Dump statistics every %d millions of ticks", dumpstats));
    bx_pc_system.register_timer_ticks(&bx_pc_system, bx_pc_system_c::dumpStatsTimer,
        (Bit64u) dumpstats * 1000000, 1 /* continuous */, 1, "dumpstats.timer");
  }
#endif

  // set up memory and CPU objects
  bx_param_num_c *bxp_memsize = SIM->get_param_num(BXPN_MEM_SIZE);
  Bit64u memSize = bxp_memsize->get64() * BX_CONST64(1024*1024);

  bx_param_num_c *bxp_host_memsize = SIM->get_param_num(BXPN_HOST_MEM_SIZE);
  Bit64u hostMemSize = bxp_host_memsize->get64() * BX_CONST64(1024*1024);

  // do not allocate more host memory than needed for emulation of guest RAM
  if (memSize < hostMemSize) hostMemSize = memSize;

  BX_MEM(0)->init_memory(memSize, hostMemSize);

  // First load the system BIOS (VGABIOS loading moved to the vga code)
  BX_MEM(0)->load_ROM(SIM->get_param_string(BXPN_ROM_PATH)->getptr(),
                      SIM->get_param_num(BXPN_ROM_ADDRESS)->get(), 0);

  // Then load the optional ROM images
  for (i=0; i<BX_N_OPTROM_IMAGES; i++) {
    sprintf(pname, "%s.%d", BXPN_OPTROM_BASE, i+1);
    base = (bx_list_c*) SIM->get_param(pname);
    if (!SIM->get_param_string("file", base)->isempty())
      BX_MEM(0)->load_ROM(SIM->get_param_string("file", base)->getptr(),
                          SIM->get_param_num("address", base)->get(), 2);
  }

  // Then load the optional RAM images
  for (i=0; i<BX_N_OPTRAM_IMAGES; i++) {
    sprintf(pname, "%s.%d", BXPN_OPTRAM_BASE, i+1);
    base = (bx_list_c*) SIM->get_param(pname);
    if (!SIM->get_param_string("file", base)->isempty())
      BX_MEM(0)->load_RAM(SIM->get_param_string("file", base)->getptr(),
                          SIM->get_param_num("address", base)->get());
  }

#if BX_SUPPORT_SMP == 0
  BX_CPU(0)->initialize();
  BX_CPU(0)->sanity_checks();
  BX_CPU(0)->register_state();
  BX_INSTR_INITIALIZE(0);
#else
  bx_cpu_array = new BX_CPU_C_PTR[BX_SMP_PROCESSORS];

  for (unsigned i=0; i<BX_SMP_PROCESSORS; i++) {
    BX_CPU(i) = new BX_CPU_C(i);
    BX_CPU(i)->initialize();  // assign local apic id in 'initialize' method
    BX_CPU(i)->sanity_checks();
    BX_CPU(i)->register_state();
    BX_INSTR_INITIALIZE(i);
  }
#endif

  DEV_init_devices();
  // unload optional plugins which are unused and marked for removal
  SIM->opt_plugin_ctrl("*", 0);
  bx_pc_system.register_state();
  DEV_register_state();
  if (!SIM->get_param_bool(BXPN_RESTORE_FLAG)->get()) {
    bx_set_log_actions_by_device(1);
  }

  // will enable A20 line and reset CPU and devices
  bx_pc_system.Reset(BX_RESET_HARDWARE);

  if (SIM->get_param_bool(BXPN_RESTORE_FLAG)->get()) {
    if (SIM->restore_hardware()) {
      if (!SIM->restore_logopts()) {
        BX_PANIC(("cannot restore log options"));
        SIM->get_param_bool(BXPN_RESTORE_FLAG)->set(0);
      }
      bx_sr_after_restore_state();
    } else {
      BX_PANIC(("cannot restore hardware state"));
      SIM->get_param_bool(BXPN_RESTORE_FLAG)->set(0);
    }
  }

  bx_gui->init_signal_handlers();
  bx_pc_system.start_timers();

  BX_DEBUG(("bx_init_hardware is setting signal handlers"));
// if not using debugger, then we can take control of SIGINT.
#if !BX_DEBUGGER
  signal(SIGINT, bx_signal_handler);
#endif

#if BX_SHOW_IPS
#if !defined(WIN32)
  if (!SIM->is_wx_selected()) {
    signal(SIGALRM, bx_signal_handler);
    alarm(1);
  }
#endif
#endif
}

void bx_init_bx_dbg(void)
{
#if BX_DEBUGGER
  bx_dbg_init_infile();
#endif
  memset(&bx_dbg, 0, sizeof(bx_debug_t));
}

int bx_atexit(void)
{
  if (!SIM->get_init_done()) return 1; // protect from reentry

  // in case we ended up in simulation mode, change back to config mode
  // so that the user can see any messages left behind on the console.
  SIM->set_display_mode(DISP_MODE_CONFIG);

#if BX_DEBUGGER == 0
  if (SIM && SIM->get_init_done()) {
    for (int cpu=0; cpu<BX_SMP_PROCESSORS; cpu++)
#if BX_SUPPORT_SMP
      if (BX_CPU(cpu))
#endif
        BX_CPU(cpu)->atexit();
  }
#endif

  BX_MEM(0)->cleanup_memory();

  bx_pc_system.exit();

  // restore signal handling to defaults
#if BX_DEBUGGER == 0
  BX_INFO(("restoring default signal behavior"));
  signal(SIGINT, SIG_DFL);
#endif

#if BX_SHOW_IPS
#if !defined(__MINGW32__) && !defined(_MSC_VER)
  if (!SIM->is_wx_selected()) {
    alarm(0);
    signal(SIGALRM, SIG_DFL);
  }
#endif
#endif

  SIM->cleanup_save_restore();
  SIM->cleanup_statistics();
  SIM->set_init_done(0);

  return 0;
}

#if BX_SHOW_IPS
void bx_show_ips_handler(void)
{
  static Bit64u ticks_count = 0;
  static Bit64u counts = 0;

  // amount of system ticks passed from last time the handler was called
  Bit64u ips_count = bx_pc_system.time_ticks() - ticks_count;
  if (ips_count) {
    bx_gui->show_ips((Bit32u) ips_count);
    ticks_count = bx_pc_system.time_ticks();
    counts++;
    if (bx_dbg.print_timestamps) {
      printf("IPS: %u\taverage = %u\t\t(%us)\n",
         (unsigned) ips_count, (unsigned) (ticks_count/counts), (unsigned) counts);
      fflush(stdout);
    }
  }
  return;
}
#endif

void CDECL bx_signal_handler(int signum)
{
  // in a multithreaded environment, a signal such as SIGINT can be sent to all
  // threads.  This function is only intended to handle signals in the
  // simulator thread.  It will simply return if called from any other thread.
  // Otherwise the BX_PANIC() below can be called in multiple threads at
  // once, leading to multiple threads trying to display a dialog box,
  // leading to GUI deadlock.
  if (!SIM->is_sim_thread()) {
    BX_INFO(("bx_signal_handler: ignored sig %d because it wasn't called from the simulator thread", signum));
    return;
  }
#if BX_GUI_SIGHANDLER
  if (bx_gui_sighandler) {
    // GUI signal handler gets first priority, if the mask says it's wanted
    if ((1<<signum) & bx_gui->get_sighandler_mask()) {
      bx_gui->sighandler(signum);
      return;
    }
  }
#endif

#if BX_SHOW_IPS
  if (signum == SIGALRM) {
    bx_show_ips_handler();
#if !defined(WIN32)
    if (!SIM->is_wx_selected()) {
      signal(SIGALRM, bx_signal_handler);
      alarm(1);
    }
#endif
    return;
  }
#endif

#if BX_GUI_SIGHANDLER
  if (bx_gui_sighandler) {
    if ((1<<signum) & bx_gui->get_sighandler_mask()) {
      bx_gui->sighandler(signum);
      return;
    }
  }
#endif

  BX_PANIC(("SIGNAL %u caught", signum));
}
