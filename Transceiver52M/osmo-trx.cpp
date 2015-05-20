/*
 * Copyright (C) 2013 Thomas Tsou <tom@tsou.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "Transceiver.h"
#include "radioDevice.h"

#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <GSMCommon.h>
#include <Logger.h>
#include <Configuration.h>

/* Samples-per-symbol for downlink path
 *     4 - Uses precision modulator (more computation, less distortion)
 *     1 - Uses minimized modulator (less computation, more distortion)
 *
 *     Other values are invalid. Receive path (uplink) is always
 *     downsampled to 1 sps. Default to 4 sps for all cases except for
 *     ARM and non-SIMD enabled architectures.
 */
#if defined(HAVE_NEON) || !defined(HAVE_SSE3)
#define DEFAULT_SPS		1
#else
#define DEFAULT_SPS		4
#endif

/* Default configuration parameters
 *     Note that these values are only used if the particular key does not
 *     exist in the configuration database. IP port and address values will
 *     typically be overwritten by the OpenBTS.db values. Other values will
 *     not be in the database by default.
 */
#define DEFAULT_TRX_PORT	5700
#define DEFAULT_TRX_IP		"127.0.0.1"
#define DEFAULT_EXTREF		false
#define DEFAULT_DIVERSITY	false
#define DEFAULT_CHANS		1

/* Amplitude less than 0.0 triggers device specific default */
#define DEFAULT_AMPL		-9999.0

struct trx_config {
	std::string log_level;
	std::string addr;
	std::string dev_args;
	unsigned port;
	unsigned sps;
	unsigned chans;
	unsigned rtsc;
	int filler;
	bool extref;
	bool diversity;
	double offset;
	double ampl;
};

ConfigurationTable gConfig;

volatile bool gshutdown = false;

/* Run sanity check on configuration table
 *     The global table constructor cannot provide notification in the
 *     event of failure. Make sure that we can access the database,
 *     write to it, and that it contains the bare minimum required keys.
 */
bool testConfig()
{
	int val = 9999;
	std::string test = "asldfkjsaldkf";
	const char *key = "Log.Level";

	/* Attempt to query */
	try {
		gConfig.getStr(key);
	} catch (...) {
		std::cerr << std::endl;
		std::cerr << "Config: Failed query required key " << key
			  << std::endl;
		return false;
	}

	/* Attempt to set a test value in the global config */
	if (!gConfig.set(test, val)) {
		std::cerr << std::endl;
		std::cerr << "Config: Failed to set test key" << std::endl;
		return false;
	} else {
		gConfig.remove(test);
	}

	return true;
}


/* Setup configuration values
 *     Don't query the existence of the Log.Level because it's a
 *     mandatory value. That is, if it doesn't exist, the configuration
 *     table will crash or will have already crashed. Everything else we
 *     can survive without and use default values if the database entries
 *     are empty.
 */
bool trx_setup_config(struct trx_config *config)
{
	std::string refstr, fillstr, divstr;

	if (!testConfig())
		return false;

	if (config->log_level == "")
		config->log_level = gConfig.getStr("Log.Level");

	if (!config->port) {
		if (gConfig.defines("TRX.Port"))
			config->port = gConfig.getNum("TRX.Port");
		else
			config->port = DEFAULT_TRX_PORT;
	}

	if (config->addr == "") {
		if (gConfig.defines("TRX.IP"))
			config->addr = gConfig.getStr("TRX.IP");
		else
			config->addr = DEFAULT_TRX_IP;
	}

	if (!config->extref) {
		if (gConfig.defines("TRX.Reference"))
			config->extref = gConfig.getNum("TRX.Reference");
		else
			config->extref = DEFAULT_EXTREF;
	}

	if (!config->diversity) {
		if (gConfig.defines("TRX.Diversity"))
			config->diversity = gConfig.getNum("TRX.Diversity");
		else
			config->diversity = DEFAULT_DIVERSITY;
	}

	/* Diversity only supported on 2 channels */
	if (config->diversity)
		config->chans = 2;

	refstr = config->extref ? "Enabled" : "Disabled";
	fillstr = config->filler ? "Enabled" : "Disabled";
	divstr = config->diversity ? "Enabled" : "Disabled";

	std::ostringstream ost("");
	ost << "Config Settings" << std::endl;
	ost << "   Log Level............... " << config->log_level << std::endl;
	ost << "   Device args............. " << config->dev_args << std::endl;
	ost << "   TRX Base Port........... " << config->port << std::endl;
	ost << "   TRX Address............. " << config->addr << std::endl;
	ost << "   Channels................ " << config->chans << std::endl;
	ost << "   Samples-per-Symbol...... " << config->sps << std::endl;
	ost << "   External Reference...... " << refstr << std::endl;
	ost << "   C0 Filler Table......... " << fillstr << std::endl;
	ost << "   Diversity............... " << divstr << std::endl;
	ost << "   Tuning offset........... " << config->offset << std::endl;
	ost << "   Sample amplitude........ " << config->ampl << std::endl;
	std::cout << ost << std::endl;

	return true;
}

/* Create radio interface
 *     The interface consists of sample rate changes, frequency shifts,
 *     channel multiplexing, and other conversions. The transceiver core
 *     accepts input vectors sampled at multiples of the GSM symbol rate.
 *     The radio interface connects the main transceiver with the device
 *     object, which may be operating some other rate.
 */
RadioInterface *makeRadioInterface(struct trx_config *config,
                                   RadioDevice *usrp, int type)
{
	RadioInterface *radio = NULL;

	switch (type) {
	case RadioDevice::NORMAL:
		radio = new RadioInterface(usrp, config->sps, config->chans);
		break;
	case RadioDevice::RESAMP_64M:
	case RadioDevice::RESAMP_100M:
		radio = new RadioInterfaceResamp(usrp,
						 config->sps, config->chans);
		break;
	case RadioDevice::DIVERSITY:
		radio = new RadioInterfaceDiversity(usrp,
						    config->sps, config->chans);
		break;
	default:
		LOG(ALERT) << "Unsupported radio interface configuration";
		return NULL;
	}

	if (!radio->init(type)) {
		LOG(ALERT) << "Failed to initialize radio interface";
		return NULL;
	}

	return radio;
}

/* Create transceiver core
 *     The multi-threaded modem core operates at multiples of the GSM rate of
 *     270.8333 ksps and consists of GSM specific modulation, demodulation,
 *     and decoding schemes. Also included are the socket interfaces for
 *     connecting to the upper layer stack.
 */
Transceiver *makeTransceiver(struct trx_config *config, RadioInterface *radio)
{
	Transceiver *trx;
	VectorFIFO *fifo;

	trx = new Transceiver(config->port, config->addr.c_str(), config->sps,
			      config->chans, GSM::Time(3,0), radio);
	if (!trx->init(config->filler, config->rtsc)) {
		LOG(ALERT) << "Failed to initialize transceiver";
		delete trx;
		return NULL;
	}

	for (size_t i = 0; i < config->chans; i++) {
		fifo = radio->receiveFIFO(i);
		if (fifo && trx->receiveFIFO(fifo, i))
			continue;

		LOG(ALERT) << "Could not attach FIFO to channel " << i;
		delete trx;
		return NULL;
	}

	return trx;
}

static void sig_handler(int signo)
{
	fprintf(stdout, "Received shutdown signal");
	gshutdown = true;
}

static void setup_signal_handlers()
{
	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		fprintf(stderr, "Failed to install SIGINT signal handler\n");
		exit(EXIT_FAILURE);
	}
	if (signal(SIGTERM, sig_handler) == SIG_ERR) {
		fprintf(stderr, "Couldn't install SIGTERM signal handler\n");
		exit( EXIT_FAILURE);
	}
}

static void print_help()
{
	fprintf(stdout, "Options:\n"
		"  -h    This text\n"
		"  -a    UHD device args\n"
		"  -l    Logging level (%s)\n"
		"  -i    IP address of GSM core\n"
		"  -p    Base port number\n"
		"  -d    Enable dual channel diversity receiver\n"
		"  -x    Enable external 10 MHz reference\n"
		"  -s    Samples-per-symbol (1 or 4)\n"
		"  -c    Number of ARFCN channels (default=1)\n"
		"  -f    Enable C0 filler table\n"
		"  -o    Set baseband frequency offset (default=auto)\n"
		"  -m    Set sample amplitude (default=0.3)\n"
		"  -r    Random burst test mode with TSC (0 to 7)\n",
		"EMERG, ALERT, CRT, ERR, WARNING, NOTICE, INFO, DEBUG");
}

static void handle_options(int argc, char **argv, struct trx_config *config)
{
	int option;

	config->port = 0;
	config->sps = DEFAULT_SPS;
	config->chans = DEFAULT_CHANS;
	config->rtsc = 0;
	config->extref = false;
	config->filler = Transceiver::FILLER_ZERO;
	config->diversity = false;
	config->offset = 0.0;
	config->ampl = DEFAULT_AMPL;

	while ((option = getopt(argc, argv, "ha:l:i:p:c:dxfo:s:r:m:")) != -1) {
		switch (option) {
		case 'h':
			print_help();
			exit(0);
			break;
		case 'a':
			config->dev_args = optarg;
			break;
		case 'l':
			config->log_level = optarg;
			break;
		case 'i':
			config->addr = optarg;
			break;
		case 'p':
			config->port = atoi(optarg);
			break;
		case 'c':
			config->chans = atoi(optarg);
			break;
		case 'd':
			config->diversity = true;
			break;
		case 'x':
			config->extref = true;
			break;
		case 'f':
			config->filler = Transceiver::FILLER_DUMMY;
			break;
		case 'o':
			config->offset = atof(optarg);
			break;
		case 's':
			config->sps = atoi(optarg);
			break;
		case 'r':
			config->rtsc = atoi(optarg);
			config->filler = Transceiver::FILLER_RAND;
			break;
		case 'm':
			config->ampl = atof(optarg);
			break;
		default:
			print_help();
			exit(0);
		}
	}

	if ((config->sps != 1) && (config->sps != 4)) {
		printf("Unsupported samples-per-symbol %i\n\n", config->sps);
		print_help();
		exit(0);
	}

	if (config->rtsc > 7) {
		printf("Invalid training sequence %i\n\n", config->rtsc);
		print_help();
		exit(0);
	}
}

int main(int argc, char *argv[])
{
	int type, chans;
	RadioDevice *usrp;
	RadioInterface *radio = NULL;
	Transceiver *trx = NULL;
	struct trx_config config;

	handle_options(argc, argv, &config);

	setup_signal_handlers();

	/* Check database sanity */
	if (!trx_setup_config(&config)) {
		std::cerr << "Config: Database failure - exiting" << std::endl;
		return EXIT_FAILURE;
	}

	gLogInit("transceiver", config.log_level.c_str(), LOG_LOCAL7);

	srandom(time(NULL));

	/* Create the low level device object */
	usrp = RadioDevice::make(config.sps, config.chans, config.ampl,
				 config.diversity, config.offset);
	type = usrp->open(config.dev_args, config.extref);
	if (type < 0) {
		LOG(ALERT) << "Failed to create radio device" << std::endl;
		goto shutdown;
	}

	/* Setup the appropriate device interface */
	radio = makeRadioInterface(&config, usrp, type);
	if (!radio)
		goto shutdown;

	/* Create the transceiver core */
	trx = makeTransceiver(&config, radio);
	if (!trx)
		goto shutdown;

	chans = trx->numChans();
	std::cout << "-- Transceiver active with "
		  << chans << " channel(s)" << std::endl;

	while (!gshutdown)
		sleep(1);

shutdown:
	std::cout << "Shutting down transceiver..." << std::endl;

	delete trx;
	delete radio;
	delete usrp;

	return 0;
}
