#include <climits>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <vector>
#define __STDC_FORMAT_MACROS 1

#include "Error.h"
#include "Rand.h"
#include "Kernels.h"
#include "Constants.h"
#include "Dist.h"
#include "Param.h"
#include "SetupModel.h"
#include "Model.h"
#include "ModelMacros.h"
#include "InfStat.h"
#include "Bitmap.h"
#include "Memory.h"

void* BinFileBuf;
BinFile* BF;
int netbuf[MAX_NUM_PLACE_TYPES * 1000000];


///// INITIALIZE / SET UP FUNCTIONS
void SetupModel(std::string const& density_file, std::string const& out_density_file, std::string const& load_network_file,
				std::string const& save_network_file, std::string const& school_file, std::string const& reg_demog_file,
				std::string const& out_file_base)
{
	int l, m, j2, l2, m2;
	unsigned int rn;
	double t, s, s2;
	char buf[2048];
	FILE* dat;

	// allocate memory for integers used in multi=threaded random number generation.
  Xcg1 = (int32_t*)Memory::xcalloc(MAX_NUM_THREADS * CACHE_LINE_SIZE, sizeof(int32_t));
  Xcg2 = (int32_t*)Memory::xcalloc(MAX_NUM_THREADS * CACHE_LINE_SIZE, sizeof(int32_t));
	P.nextSetupSeed1 = P.setupSeed1;
	P.nextSetupSeed2 = P.setupSeed2;
	setall(&P.nextSetupSeed1, &P.nextSetupSeed2);
	P.DoBin = -1;
	if (!density_file.empty())
	{
		Files::xfprintf_stderr("Scanning population density file\n");
		dat = Files::xfopen(density_file.c_str(), "rb");
		unsigned int density_file_header;
		Files::fread_big(&density_file_header, sizeof(unsigned int), 1, dat);
		if (density_file_header == 0xf0f0f0f0) //code for first 4 bytes of binary file ## NOTE - SHOULD BE LONG LONG TO COPE WITH BIGGER POPULATIONS
		{
			P.DoBin = 1;
			Files::fread_big(&(P.BinFileLen), sizeof(unsigned int), 1, dat);
			BinFileBuf = Memory::xcalloc(P.BinFileLen, sizeof(BinFile));
			Files::fread_big(BinFileBuf, sizeof(BinFile), (size_t)P.BinFileLen, dat);
			BF = (BinFile*)BinFileBuf;
			Files::xfclose(dat);
		}
		else
		{
			P.DoBin = 0;
			// Count the number of lines in the density file
			rewind(dat);
			P.BinFileLen = 0;
			while(fgets(buf, sizeof(buf), dat) != NULL) P.BinFileLen++;
			if(ferror(dat)) ERR_CRITICAL("Error while reading density file\n");
			// Read each line, and build the binary structure that corresponds to it
			rewind(dat);
			BinFileBuf = (void*)Memory::xcalloc(P.BinFileLen, sizeof(BinFile));
			BF = (BinFile*)BinFileBuf;
			unsigned int index = 0;
			while(fgets(buf, sizeof(buf), dat) != NULL)
			{
				int i2;
				double x, y;
				// This shouldn't be able to happen, as we just counted the number of lines:
				if (index == P.BinFileLen) ERR_CRITICAL("Too many input lines while reading density file\n");
				if (P.DoAdUnits)
				{
					Files::xsscanf(buf, 5, "%lg %lg %lg %i %i", &x, &y, &t, &i2, &l);
					if (l / P.CountryDivisor != i2)
					{
						//Files::xfprintf_stderr("# %lg %lg %lg %i %i\n",x,y,t,i2,l);
					}
				}
				else {
					Files::xsscanf(buf, 4, "%lg %lg %lg %i", &x, &y, &t, &i2);
					l = 0;
				}
				// Ensure we use an x which gives us a contiguous whole for the
				// geography.
				if (x >= P.LongitudeCutLine) {
					BF[index].x = x;
				}
				else {
					BF[index].x = x + 360;
				}
				BF[index].y = y;
				BF[index].pop = t;
				BF[index].cnt = i2;
				BF[index].ad = l;
				index++;
			}
			if(ferror(dat)) ERR_CRITICAL("Error while reading density file\n");
			// This shouldn't be able to happen, as we just counted the number of lines:
			if (index != P.BinFileLen) ERR_CRITICAL("Too few input lines while reading density file\n");
			Files::xfclose(dat);
		}

		if (P.DoAdunitBoundaries)
		{
			// We will compute a precise spatial bounding box using the population locations.
			// Initially, set the min values too high, and the max values too low, and then
			// we will adjust them as we read population data.
			P.SpatialBoundingBox.reset();
			s2 = 0;
			for (rn = 0; rn < P.BinFileLen; rn++)
			{
				double x = BF[rn].x;
				double y = BF[rn].y;
				t = BF[rn].pop;
				int i2 = BF[rn].cnt;
				l = BF[rn].ad;
				//					Files::xfprintf_stderr("# %lg %lg %lg %i\t",x,y,t,l);

				m = (l % P.AdunitLevel1Mask) / P.AdunitLevel1Divisor;
				if (P.AdunitLevel1Lookup[m] >= 0)
					if (AdUnits[P.AdunitLevel1Lookup[m]].id / P.AdunitLevel1Mask == l / P.AdunitLevel1Mask)
					{
						AdUnits[P.AdunitLevel1Lookup[m]].cnt_id = i2;
						s2 += t;
						// Adjust the bounds of the spatial bounding box so that they include the location
						// for this block of population.
						P.SpatialBoundingBox.expand(CovidSim::Geometry::Vector2d(x, y));
					}
			}
			if (!P.DoSpecifyPop) P.PopSize = (int)s2;
		}

		P.in_cells_.height = P.in_cells_.width;
		P.SpatialBoundingBox.to_grid(P.in_cells_.width, P.in_cells_.height);
		P.in_degrees_.width = P.SpatialBoundingBox.width();
		P.in_degrees_.height = P.SpatialBoundingBox.height();
		P.ncw = 4 * ((int)ceil(P.in_degrees_.width / P.in_cells_.width / 4));
		P.nch = 4 * ((int)ceil(P.in_degrees_.height / P.in_cells_.height / 4));
		P.in_degrees_.width = ((double)P.ncw) * P.in_cells_.width;
		P.in_degrees_.height = ((double)P.nch) * P.in_cells_.height;
		P.SpatialBoundingBox.top_right() = P.SpatialBoundingBox.bottom_left()
			+ CovidSim::Geometry::Vector2d(P.in_degrees_.width, P.in_degrees_.height);
		P.NumCells = P.ncw * P.nch;
		Files::xfprintf_stderr("Adjusted bounding box = (%lg, %lg)- (%lg, %lg)\n",
				P.SpatialBoundingBox.bottom_left().x, P.SpatialBoundingBox.bottom_left().y,
				P.SpatialBoundingBox.top_right().x,   P.SpatialBoundingBox.top_right().y);
		Files::xfprintf_stderr("Number of cells = %i (%i x %i)\n", P.NumCells, P.ncw, P.nch);
		Files::xfprintf_stderr("Population size = %i \n", P.PopSize);
		if (P.in_degrees_.width > 180) {
			Files::xfprintf_stderr("WARNING: Width of bounding box > 180 degrees.  Results may be inaccurate.\n");
		}
		if (P.in_degrees_.height > 90) {
			Files::xfprintf_stderr("WARNING: Height of bounding box > 90 degrees.  Results may be inaccurate.\n");
		}
		s = 1;
		P.DoPeriodicBoundaries = 0;
	}
	else
	{
		P.ncw = P.nch = (int)sqrt((double)P.NumCells);
		P.NumCells = P.ncw * P.nch;
		Files::xfprintf_stderr("Number of cells adjusted to be %i (%i^2)\n", P.NumCells, P.ncw);
		s = floor(sqrt((double)P.PopSize));
		P.SpatialBoundingBox.bottom_left() = CovidSim::Geometry::Vector2d(0.0, 0.0);
		P.SpatialBoundingBox.top_right() = CovidSim::Geometry::Vector2d(s, s);
		P.PopSize = (int)(s * s);
		Files::xfprintf_stderr("Population size adjusted to be %i (%lg^2)\n", P.PopSize, s);
		P.in_degrees_.width = P.in_degrees_.height = s;
		P.in_cells_.width = P.in_degrees_.width / ((double)P.ncw);
		P.in_cells_.height = P.in_degrees_.height / ((double)P.nch);
	}
	P.NumMicrocells = P.NMCL * P.NMCL * P.NumCells;
	P.total_microcells_wide_ = P.ncw * P.NMCL;
	P.total_microcells_high_ = P.nch * P.NMCL;
	Files::xfprintf_stderr("Number of microcells = %i\n", P.NumMicrocells);
	P.scale.x = P.BitmapScale;
	P.scale.y = P.BitmapAspectScale * P.BitmapScale;
	P.b.width = (int)(P.in_degrees_.width * (P.BoundingBox.width()) * P.scale.x);
	P.b.width = (P.b.width + 3) / 4;
	P.b.width *= 4;
	P.b.height = (int)(P.in_degrees_.height * (P.BoundingBox.height()) * P.scale.y);
	P.b.height += (4 - P.b.height % 4) % 4;
	P.bheight2 = P.b.height + 20; // space for colour legend
	Files::xfprintf_stderr("Bitmap width = %i\nBitmap height = %i\n", P.b.width, P.b.height);
	P.bmin.x = (int)(P.in_degrees_.width * P.BoundingBox.bottom_left().x * P.scale.x);
	P.bmin.y = (int)(P.in_degrees_.height * P.BoundingBox.bottom_left().y * P.scale.y);
	P.in_microcells_.width = P.in_cells_.width / ((double)P.NMCL);
	P.in_microcells_.height = P.in_cells_.height / ((double)P.NMCL);
	for (int i = 0; i < P.NumSeedLocations; i++)
	{
		P.LocationInitialInfection[i][0] -= P.SpatialBoundingBox.bottom_left().x;
		P.LocationInitialInfection[i][1] -= P.SpatialBoundingBox.bottom_left().y;
	}
	// Find longest distance - may not be diagonally across the bounding box.
	t = dist2_raw(0, 0, P.in_degrees_.width, P.in_degrees_.height);
	double tw = dist2_raw(0, 0, P.in_degrees_.width, 0);
	double th = dist2_raw(0, 0, 0, P.in_degrees_.height);
	if (tw > t) t = tw;
	if (th > t) t = th;
	if (P.DoPeriodicBoundaries) t *= 0.25;
	P.KernelLookup.setup(t);
	//	Files::xfprintf_stderr("** %i %lg %lg %lg %lg | %lg %lg %lg %lg \n",P.DoUTM_coords,P.SpatialBoundingBox[0],P.SpatialBoundingBox[1],P.SpatialBoundingBox[2],P.SpatialBoundingBox[3],P.width,P.height,t,P.KernelDelta);
	Files::xfprintf_stderr("Coords xmcell=%lg m   ymcell = %lg m\n",
		sqrt(dist2_raw(P.in_degrees_.width / 2, P.in_degrees_.height / 2, P.in_degrees_.width / 2 + P.in_microcells_.width, P.in_degrees_.height / 2)),
		sqrt(dist2_raw(P.in_degrees_.width / 2, P.in_degrees_.height / 2, P.in_degrees_.width / 2, P.in_degrees_.height / 2 + P.in_microcells_.height)));

	SetupPopulation(density_file, out_density_file, school_file, reg_demog_file);

	// allocate memory for Time-Series
	TimeSeries = (Results*)Memory::xcalloc(P.NumOutputTimeSteps, sizeof(Results));
	TSMeanE = (Results*)Memory::xcalloc(P.NumOutputTimeSteps, sizeof(Results));
	TSVarE = (Results*)Memory::xcalloc(P.NumOutputTimeSteps, sizeof(Results));
	TSMeanNE = (Results*)Memory::xcalloc(P.NumOutputTimeSteps, sizeof(Results));
	TSVarNE = (Results*)Memory::xcalloc(P.NumOutputTimeSteps, sizeof(Results));
	TSMean = TSMeanE; TSVar = TSVarE;

	// allocate memory for Time-Series age and admin unit breakdowns.
	if (P.DoAdUnits && P.OutputAdUnitAge)
	{
		State.prevInf_age_adunit = (int**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(int*));
		State.cumInf_age_adunit  = (int**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(int*));
		for (int AgeGroup = 0; AgeGroup < NUM_AGE_GROUPS; AgeGroup++)
		{
			State.prevInf_age_adunit[AgeGroup] = (int*)Memory::xcalloc(P.NumAdunits, sizeof(int));
			State.cumInf_age_adunit [AgeGroup] = (int*)Memory::xcalloc(P.NumAdunits, sizeof(int));
		}

		for (int Thread = 0; Thread < P.NumThreads; Thread++)
		{
			StateT[Thread].prevInf_age_adunit = (int**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(int*));
			StateT[Thread].cumInf_age_adunit  = (int**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(int*));
			for (int AgeGroup = 0; AgeGroup < NUM_AGE_GROUPS; AgeGroup++)
			{
				StateT[Thread].prevInf_age_adunit[AgeGroup] = (int*)Memory::xcalloc(P.NumAdunits, sizeof(int));
				StateT[Thread].cumInf_age_adunit [AgeGroup] = (int*)Memory::xcalloc(P.NumAdunits, sizeof(int));
			}
		}

		for (int Time = 0; Time < P.NumOutputTimeSteps; Time++)
		{
			TimeSeries[Time].prevInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TimeSeries[Time].incInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TimeSeries[Time].cumInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanE [Time].prevInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanE [Time].incInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanE [Time].cumInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanNE[Time].prevInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanNE[Time].incInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));
			TSMeanNE[Time].cumInf_age_adunit = (double**)Memory::xcalloc(NUM_AGE_GROUPS, sizeof(double*));

			for (int AgeGroup = 0; AgeGroup < NUM_AGE_GROUPS; AgeGroup++)
			{
				TimeSeries[Time].prevInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TimeSeries[Time].incInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TimeSeries[Time].cumInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanE[Time].prevInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanE[Time].incInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanE[Time].cumInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanNE[Time].prevInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanNE[Time].incInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
				TSMeanNE[Time].cumInf_age_adunit[AgeGroup] = (double*)Memory::xcalloc(P.NumAdunits, sizeof(double));
			}
		}
	}

	//added memory allocation and initialisation of infection event log, if DoRecordInfEvents is set to 1: ggilani - 10/10/2014
	if (P.DoRecordInfEvents)
	{
		InfEventLog = (Events*)Memory::xcalloc(P.MaxInfEvents, sizeof(Events));
	}

	if(P.OutputNonSeverity) SaveAgeDistrib(out_file_base);

	Files::xfprintf_stderr("Initialising places...\n");
	if (P.DoPlaces)
	{
		if (!load_network_file.empty())
			LoadPeopleToPlaces(load_network_file);
		else
			AssignPeopleToPlaces();
	}

	if (P.DoPlaces && !save_network_file.empty())
		SavePeopleToPlaces(save_network_file);
	//SaveDistribs();

	// From here on, we want the same random numbers regardless of whether we used the RNG to make the network,
	// or loaded the network from a file. Therefore we need to reseed the RNG.
	setall(&P.nextSetupSeed1, &P.nextSetupSeed2);

	StratifyPlaces();
	for (int i = 0; i < P.NumCells; i++)
	{
		Cells[i].S = Cells[i].n;
		Cells[i].L = Cells[i].I = Cells[i].R = 0;
		//Cells[i].susceptible=Cells[i].members; //added this line
	}

	Files::xfprintf_stderr("Initialising kernel...\n");
	P.Kernel = P.MoveKernel;
	P.KernelLookup.init(1.0, P.Kernel);
	CovidSim::TBD1::KernelLookup::init(P.KernelLookup, CellLookup, P.NumPopulatedCells);

	for (int i = 0; i < P.PopSize; i++) Hosts[i].keyworker = Hosts[i].care_home_resident = 0;
	double nstaff = 0, nres = 0;
	if ((P.CareHomePlaceType >= 0) && (P.CareHomeResidentMinimumAge < 1000))  // label care home residents as they don't have household contacts
	{
		for (int i = 0; i < P.PopSize; i++)
			if (Hosts[i].PlaceLinks[P.CareHomePlaceType] >= 0)
			{
				if (Hosts[i].age >= P.CareHomeResidentMinimumAge)
				{
					Hosts[i].care_home_resident = 1;
					nres++;
				}
				else
					nstaff++;
			}
		P.CareHomePropResidents = (nres > 0) ? (nres / (nres + nstaff)) : 0.0;
	}
	else
		P.CareHomePropResidents = 0.0;
	Files::xfprintf_stderr("%lg care home residents\n%lg care home workers\n", nres, nstaff);
	P.KeyWorkerNum = P.KeyWorkerIncHouseNum = m = l = 0;
	if (P.DoPlaces)
	{
		while ((m < P.KeyWorkerPopNum) && (l < 1000))
		{
			int i = (int)(((double)P.PopSize) * ranf_mt(0));
			if (Hosts[i].keyworker)
				l++;
			else
			{
				Hosts[i].keyworker = 1;
				m++;
				P.KeyWorkerNum++;
				P.KeyWorkerIncHouseNum++;
				l = 0;
				if (ranf_mt(0) < P.KeyWorkerHouseProp)
				{
					l2 = Households[Hosts[i].hh].FirstPerson;
					m2 = l2 + Households[Hosts[i].hh].nh;
					for (j2 = l2; j2 < m2; j2++)
						if (!Hosts[j2].keyworker)
						{
							Hosts[j2].keyworker = 1;
							P.KeyWorkerIncHouseNum++;
						}
				}
			}
		}
		for (int j = 0; j < P.PlaceTypeNoAirNum; j++)
		{
			m = l = 0;
			while ((m < P.KeyWorkerPlaceNum[j]) && (l < 1000))
			{
				int k = (int)(((double)P.Nplace[j]) * ranf_mt(0));
				for (int i2 = 0; (m < P.KeyWorkerPlaceNum[j]) && (i2 < Places[j][k].n); i2++)
				{
					int i = Places[j][k].members[i2];
					if ((i < 0) || (i >= P.PopSize)) Files::xfprintf_stderr("## %i # ", i);
					if ((Hosts[i].keyworker) || (ranf_mt(0) >= P.KeyWorkerPropInKeyPlaces[j]))
						l++;
					else
					{
						Hosts[i].keyworker = 1;
						m++;
						P.KeyWorkerNum++;
						P.KeyWorkerIncHouseNum++;
						l = 0;
						l2 = Households[Hosts[i].hh].FirstPerson;
						m2 = l2 + Households[Hosts[i].hh].nh;
						for (j2 = l2; j2 < m2; j2++)
							if ((!Hosts[j2].keyworker) && (ranf_mt(0) < P.KeyWorkerHouseProp))
							{
								Hosts[j2].keyworker = 1;
								P.KeyWorkerIncHouseNum++;
							}
					}
				}
			}
		}
		if (P.KeyWorkerNum > 0) Files::xfprintf_stderr("%i key workers selected in total\n", P.KeyWorkerNum);
		if (P.DoAdUnits)
		{
			for (int i = 0; i < P.NumAdunits; i++) AdUnits[i].NP = 0;
			for (int j = 0; j < P.NumPlaceTypes; j++)
				if (P.PlaceCloseAdunitPlaceTypes[j] > 0)
				{
					for (int k = 0; k < P.Nplace[j]; k++)
						AdUnits[Mcells[Places[j][k].mcell].adunit].NP++;
				}
		}
	}
	Files::xfprintf_stderr("Places intialised.\n");

	//Set up the population for digital contact tracing here... - ggilani 09/03/20
	if (P.DoDigitalContactTracing)
	{
		P.NDigitalContactUsers = 0;
		l = m=0;
		//if clustering by Households
		if (P.DoHouseholds && P.ClusterDigitalContactUsers)
		{
			//Loop through households

			//NOTE: Are we still okay with this kind of openmp parallelisation. I know there have been some discussions re:openmp, but not followed them completely
			l = m = 0;
#pragma omp parallel for schedule(static,1) reduction(+:l,m) default(none) \
				shared(P, Households, Hosts)
			for (int tn = 0; tn < P.NumThreads; tn++)
			{
				for (int i = tn; i < P.NumHouseholds; i += P.NumThreads)
				{
					if (ranf_mt(tn) < P.PropPopUsingDigitalContactTracing)
					{
						//select this household for digital contact app use
						//loop through household members and check whether they will be selected for use
						int i1 = Households[i].FirstPerson;
						int i2 = i1 + Households[i].nh;
						for (int j = i1; j < i2; j++)
						{
							//get age of host
							int age = HOST_AGE_GROUP(j);
							if (age >= NUM_AGE_GROUPS) age = NUM_AGE_GROUPS - 1;
							//check to see if host will be a user based on age group
							if (ranf_mt(tn) < P.ProportionSmartphoneUsersByAge[age])
							{
								Hosts[j].digitalContactTracingUser = 1;
								l++;
							}
						}
						m++;
					}
				}
			}
			P.NDigitalContactUsers = l;
			P.NDigitalHouseholdUsers = m;
			Files::xfprintf_stderr("Number of digital contact tracing households: %i, out of total number of households: %i\n", P.NDigitalHouseholdUsers, P.NumHouseholds);
			Files::xfprintf_stderr("Number of digital contact tracing users: %i, out of population size: %i\n", P.NDigitalContactUsers, P.PopSize);
		}
		else // Just go through the population and assign people to the digital contact tracing app based on probability by age.
		{
			//for use with non-clustered
			l = 0;
#pragma omp parallel for schedule(static,1) reduction(+:l) default(none) \
				shared(P, Hosts)
			for (int tn = 0; tn < P.NumThreads; tn++)
			{
				for (int i = tn; i < P.PopSize; i += P.NumThreads)
				{
					int age = HOST_AGE_GROUP(i);
					if (age >= NUM_AGE_GROUPS) age = NUM_AGE_GROUPS - 1;

					if (ranf_mt(tn) < (P.ProportionSmartphoneUsersByAge[age] * P.PropPopUsingDigitalContactTracing))
					{
						Hosts[i].digitalContactTracingUser = 1;
						l++;
					}
				}
			}
			P.NDigitalContactUsers = l;
			Files::xfprintf_stderr("Number of digital contact tracing users: %i, out of population size: %i\n", P.NDigitalContactUsers, P.PopSize);
		}
	}

	UpdateProbs(0);
	if (P.DoAirports) SetupAirports();

	TSMean = TSMeanNE; TSVar = TSVarNE;
	Files::xfprintf_stderr("Calculated approx cell probabilities\n");
	for (int i = 0; i < INFECT_TYPE_MASK; i++) inftype_av[i] = 0;
	for (int i = 0; i < MAX_COUNTRIES; i++) infcountry_av[i] = infcountry_num[i] = 0;
	for (int i = 0; i < MAX_SEC_REC; i++)
		for (int j = 0; j < MAX_GEN_REC; j++)
			indivR0_av[i][j] = 0;
	for (int i = 0; i <= MAX_HOUSEHOLD_SIZE; i++)
		for (int j = 0; j <= MAX_HOUSEHOLD_SIZE; j++)
			inf_household_av[i][j] = case_household_av[i][j] = 0;
	DoInitUpdateProbs = 1;
	for (int i = 0; i < P.NumCells; i++)	Cells[i].tot_treat = 1;  //This makes sure InitModel intialises the cells.
	P.NRactE = P.NRactNE = 0;
	for (int i = 0; i < P.PopSize; i++) Hosts[i].esocdist_comply = (ranf() < P.EnhancedSocDistProportionCompliant[HOST_AGE_GROUP(i)]) ? 1 : 0;
	if (P.EnhancedSocDistClusterByHousehold)
	{
		for (int i = 0; i < P.NumHouseholds; i++)
		{
			l = Households[i].FirstPerson;
			m = l + Households[i].nh;
			int i2 = 0;
			for (int k = l; k < m; k++) {
				if (Hosts[k].esocdist_comply) i2 = 1;
			}
			if (i2)
			{
				for (int k = l; k < m; k++) Hosts[k].esocdist_comply = 1;
			}
		}
	}

	if (P.OutputBitmap)
	{
		InitBMHead(out_file_base);
	}
	if (P.DoMassVacc)
	{
		State.mvacc_queue = (int*) Memory::xcalloc(P.PopSize, sizeof(int));
		int queueIndex = 0;
		for (int i = 0; i < P.PopSize; i++)
		{
			if ((HOST_AGE_YEAR(i) >= P.VaccPriorityGroupAge[0]) && (HOST_AGE_YEAR(i) <= P.VaccPriorityGroupAge[1]))
			{
				if (ranf() < P.VaccProp)
					State.mvacc_queue[queueIndex++] = i;
			}
		}
		int vaccineCount = queueIndex;
		for (int i = 0; i < P.PopSize; i++)
		{
			if ((HOST_AGE_YEAR(i) < P.VaccPriorityGroupAge[0]) || (HOST_AGE_YEAR(i) > P.VaccPriorityGroupAge[1]))
			{
				if (ranf() < P.VaccProp)
					State.mvacc_queue[queueIndex++] = i;
			}
		}
		State.n_mvacc = queueIndex;
		Files::xfprintf_stderr("Number to be vaccinated=%i\n", State.n_mvacc);
		for (int i = 0; i < 2; i++)
		{
			for (int j = 0; j < vaccineCount; j++)
			{
				l = (int)(ranf() * ((double)vaccineCount));
				m = State.mvacc_queue[j];
				State.mvacc_queue[j] = State.mvacc_queue[l];
				State.mvacc_queue[l] = m;
			}
			for (int j = vaccineCount; j < State.n_mvacc; j++)
			{
				l = vaccineCount + ((int)(ranf() * ((double)(_I64(State.n_mvacc) - vaccineCount))));
				m = State.mvacc_queue[j];
				State.mvacc_queue[j] = State.mvacc_queue[l];
				State.mvacc_queue[l] = m;
			}
		}
		Files::xfprintf_stderr("Configured mass vaccination queue.\n");
	}
	PeakHeightSum = PeakHeightSS = PeakTimeSum = PeakTimeSS = 0;
	int i = (P.ncw / 2) * P.nch + P.nch / 2;
	int j = (P.ncw / 2 + 2) * P.nch + P.nch / 2;
	Files::xfprintf_stderr("UTM dist horiz=%lg %lg\n", sqrt(dist2_cc(Cells + i, Cells + j)), sqrt(dist2_cc(Cells + j, Cells + i)));
	j = (P.ncw / 2) * P.nch + P.nch / 2 + 2;
	Files::xfprintf_stderr("UTM dist vert=%lg %lg\n", sqrt(dist2_cc(Cells + i, Cells + j)), sqrt(dist2_cc(Cells + j, Cells + i)));
	j = (P.ncw / 2 + 2) * P.nch + P.nch / 2 + 2;
	Files::xfprintf_stderr("UTM dist diag=%lg %lg\n", sqrt(dist2_cc(Cells + i, Cells + j)), sqrt(dist2_cc(Cells + j, Cells + i)));

	//if(P.OutputBitmap)
	//{
	//	CaptureBitmap();
	//	OutputBitmap(0);
	//}
	Files::xfprintf_stderr("Model configuration complete.\n");
}

void ResetTimeSeries()
{
	TSMean = TSMeanE; TSVar = TSVarE;
	///// This loops over index l twice just to reset the pointer TSMean from TSMeanE to TSMeanNE (same for TSVar).
	int num_in_results = sizeof(Results) / sizeof(double);
	for (int l = 0; l < 2; l++)
	{
		for (int i = 0; i < P.NumOutputTimeSteps; i++)
		{
			double* ts_mean = (double*)&TSMean[i];
			double* ts_var = (double*)&TSVar[i];
			for (int j = 0; j < num_in_results; j++) ts_mean[j] = ts_var[j] = 0.0;
		}
		TSMean = TSMeanNE; TSVar = TSVarNE;
	}

}

int ReadFitIter(std::string const& FitFile)
{
	/*
	Purpose of this function is:
		i) read and return a flag from FitFile determining whether to continue fitting;
		ii) if continuing fitting, to amend parameters (via command line params P.clP) from a fit file (for a particular iteration/update/proposed posterior sample from CovidSimMCMC)
	*/

	// Compare with functions CovidSimMCMC::StartJobs and CovidSimMCMC::EndJobs

	FILE* FitFile_Iter_dat;
	int PosteriorSampleNumber, NumFittedParams, cl_index[100];
	double Clock_1, Clock_2;

	std::string fit_file_iter_filename = FitFile + ".f" + std::to_string(P.FitIter) + ".txt";
	P.clP[99] = -1; // CLP #99 reserved for fitting overdispersion in likelihood.

	// Have program wait (indefinitely) until FitFile_Iter_dat / fit_file_iter_filename appears.
	do
	{
		Clock_1 = ((double) clock()) / CLOCKS_PER_SEC;
		do
		{
			Clock_2 = ((double)clock()) / CLOCKS_PER_SEC;
		}
		while ((Clock_2 > Clock_1) && (Clock_2 < Clock_1 + 1.0)); // first condition by definition true on first go of inner do-while, won't be true for very long if inner do-while not called and Clock_2 not reset, which it will be if more than 1 second has elapsed between setting of Clock_2 and resetting of Clock_1. 
	} while (!(FitFile_Iter_dat = Files::xfopen_if_exists(fit_file_iter_filename.c_str(), "r"))); // if fit_file_iter_filename exists, proceed. Otherwise go through outer and inner do-while's again. 

	// Extract iteration/posterior sample number, and number of fitted parameters from FitFile_Iter_dat
	Files::xfscanf(FitFile_Iter_dat, 2, "%i %i", &PosteriorSampleNumber, &NumFittedParams);

	// Output any errors to stderrr.
	// NumFittedParams < 0 is flag set in CovidSimMCMC::EndJobs.
	if (NumFittedParams <= 0)
		Files::xfprintf_stderr("Stop code read from file (NumFittedParams <= 0)\n");
	else if (PosteriorSampleNumber != P.FitIter)
		Files::xfprintf_stderr("Warning: iteration number %i in %s does not match file name iteration %i\n", PosteriorSampleNumber, fit_file_iter_filename.c_str(), P.FitIter);

	// assign proposed parameter set from CovidSimMCMC fitfile to parameters in CovidSim.
	if (NumFittedParams > 0)
	{
		for (int ParamNum = 0; ParamNum < NumFittedParams; ParamNum++) Files::xfscanf(FitFile_Iter_dat, 1, "%i"	, &(cl_index[ParamNum])		); // extract indices of parameters to fit (GlobalID and LocalID in CovidSimMCMC)
		for (int ParamNum = 0; ParamNum < NumFittedParams; ParamNum++) Files::xfscanf(FitFile_Iter_dat, 1, "%lg", &P.clP[cl_index[ParamNum]]); // update values in clP array at those indices (proposedParams[Region][Run][ParamNumber] in CovidSimMCMC)
	}																						
	Files::xfclose(FitFile_Iter_dat);

	// continue fitting (0) or stop (1)
	return (NumFittedParams > 0) ? 0 : 1;
}

void InitTransmissionCoeffs(void)
{
	// To calibrate R0 and various transmission coefficients/betas, effectivey run the model, (more-or-less) deterministically through the population WITHOUT any interventions. Asks how many secondary infections there would be, given infectious period, per infection at household, place and spatial levels. 
	double NumPeopleInPlaceGroup; 
	double ProbSurvive, ProbSurviveNonCareHome;
	double quantile, LatentToSympDelay;
	double HH_Infections = 0; // total number of spatial infections summed over entire population in #pragma loop below.
	double SpatialInfections = 0; // total number of spatial infections summed over entire population in #pragma loop below.
	double PlaceInfections = 0; // total number of place infections (and all place types) summed over entire population in #pragma loop below

	double Household_Infectiousness = 0; /// Household infectiousness
	double Spatial_Infectiousness = 0; /// Household infectiousness
	double Place_Infectiousness = 0;

	double HouseholdMeanSize = 0;
	double CumulativeHHSizeDist = 0; 
	// calculate weighted average of household size
	for (int HH_Size = 0; HH_Size < MAX_HOUSEHOLD_SIZE; HH_Size++)
	{
		HouseholdMeanSize += ((double)(INT64_C(1) + HH_Size)) * (P.HouseholdSizeDistrib[0][HH_Size] - CumulativeHHSizeDist); // note here that HouseholdSizeDistrib is the cumulative distribution.
		CumulativeHHSizeDist = P.HouseholdSizeDistrib[0][HH_Size];
	}
	Files::xfprintf_stderr("Household mean size = %lg\n", HouseholdMeanSize);

	//// Loops below sum household and spatial infections 
	double HH_SAR_Denom = 0.0; // household secondary-attack rate denominator. Will sum over following #pragma loop
#pragma omp parallel for private(Household_Infectiousness,Spatial_Infectiousness,quantile,LatentToSympDelay,ProbSurvive) schedule(static,1) reduction(+:HH_Infections,SpatialInfections,HH_SAR_Denom) default(none) shared(P, Households, Hosts, Mcells)
	for (int Thread = 0; Thread < P.NumThreads; Thread++) // loop over threads
	{
		for (int Person = Thread; Person < P.PopSize; Person += P.NumThreads) // loop over people
		{
			// assign susceptibility of each host.
			if (P.SusceptibilitySD == 0)
				Hosts[Person].susc = (float)((P.DoPartialImmunity) ? (1.0 - P.InitialImmunity[HOST_AGE_GROUP(Person)]) : 1.0);
			else
				Hosts[Person].susc = (float)(((P.DoPartialImmunity) ? (1.0 - P.InitialImmunity[HOST_AGE_GROUP(Person)]) : 1.0) * gen_gamma_mt(1 / (P.SusceptibilitySD * P.SusceptibilitySD), 1 / (P.SusceptibilitySD * P.SusceptibilitySD), Thread));

			// assign infectiousness of each host.
			if (P.InfectiousnessSD == 0)
				Hosts[Person].infectiousness = (float)P.AgeInfectiousness[HOST_AGE_GROUP(Person)];
			else
				Hosts[Person].infectiousness = (float)(P.AgeInfectiousness[HOST_AGE_GROUP(Person)] * gen_gamma_mt(1 / (P.InfectiousnessSD * P.InfectiousnessSD), 1 / (P.InfectiousnessSD * P.InfectiousnessSD), Thread));

			// scale infectiousness by symptomatic or asymptomatic multiplier
			if (ranf_mt(Thread) < P.ProportionSymptomatic[HOST_AGE_GROUP(Person)])	// if symptomatic, scale by Symptomatic Infectiousness (and make negative)...
				Hosts[Person].infectiousness *= (float)(-P.SymptInfectiousness);
			else					// ... or if asymptomatic
				Hosts[Person].infectiousness *= (float)P.AsymptInfectiousness;

			// choose recovery_or_death_time from infectious period quantiles (inverse cumulative distribution function). Will reset this later for each person in Update::DoIncub.
			int j = (int)floor((quantile = ranf_mt(Thread) * CDF_RES));
			quantile -= ((double)j);
			Hosts[Person].recovery_or_death_time = (unsigned short int) floor(0.5 - (P.InfectiousPeriod * log(quantile * P.infectious_icdf[j + 1] + (1.0 - quantile) * P.infectious_icdf[j]) / P.ModelTimeStep));

			// ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** 
			// ** // ** Household Infections

			if (P.DoHouseholds) // code block effectively same as household infections in InfectSweep
			{
				// choose multiplier of infectiousness
				if (P.NoInfectiousnessSDinHH)
					Household_Infectiousness = ((Hosts[Person].infectiousness < 0) ? P.SymptInfectiousness : P.AsymptInfectiousness);
				else
					Household_Infectiousness = fabs(Hosts[Person].infectiousness);
				// Care home residents less likely to infect via "household" contacts.
				if (Hosts[Person].care_home_resident) Household_Infectiousness *= P.CareHomeResidentHouseholdScaling;
				Household_Infectiousness *= P.ModelTimeStep * P.HouseholdTrans * P.HouseholdDenomLookup[Households[Hosts[Person].hh].nhr - 1];
				ProbSurvive = 1.0;
				for (int InfectiousDay = 0; InfectiousDay < (int)Hosts[Person].recovery_or_death_time; InfectiousDay++) // loop over days adding to force of infection, probability that other household members will be infected.
				{ 
					double ProbSurviveToday = 1.0 - Household_Infectiousness * P.infectiousness[InfectiousDay];
					ProbSurvive *= ((ProbSurviveToday < 0) ? 0 : ProbSurviveToday);
				}

				// loop over people in households. If household member susceptible (they will be unless already infected in this code block), 
				// and ensuring person doesn't infect themselves, add to household infections, taking account of their age and whether they're a care home resident, 
				// Person.e. the usual stuff in CalcInfSusc.cpp, but without interventions
				for (int HouseholdMember = Households[Hosts[Person].hh].FirstPerson; HouseholdMember < Households[Hosts[Person].hh].FirstPerson + Households[Hosts[Person].hh].nh; HouseholdMember++)
					if ((Hosts[HouseholdMember].is_susceptible()) && (HouseholdMember != Person))
						HH_Infections += (1 - ProbSurvive) * P.AgeSusceptibility[HOST_AGE_GROUP(Person)] * ((Hosts[HouseholdMember].care_home_resident) ? P.CareHomeResidentHouseholdScaling : 1.0);
				HH_SAR_Denom += (double)(Households[Hosts[Person].hh].nhr - 1); // add to household denominator
			}

			// ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** 
			// ** // ** Spatial Infections

			// Sum over number of days until recovery time, in two parts: entire infection and after symptoms occur, as spatial contact rate differs between these periods.
			LatentToSympDelay = (P.LatentToSymptDelay > Hosts[Person].recovery_or_death_time * P.ModelTimeStep) ? Hosts[Person].recovery_or_death_time * P.ModelTimeStep : P.LatentToSymptDelay;
			// Care home residents less likely to infect via "spatial" contacts. This doesn't correct for non care home residents being less likely to infect care home residents,
			// but since the latter are a small proportion of the population, this is a minor issue
			Spatial_Infectiousness = fabs(Hosts[Person].infectiousness) * P.RelativeSpatialContact[HOST_AGE_GROUP(Person)] * ((Hosts[Person].care_home_resident) ? P.CareHomeResidentSpatialScaling : 1.0) * P.ModelTimeStep;
			if (P.Got_WAIFW_Matrix_Spatial)
			{
				// if doing contact matrices, then need to scale spatial infections of this person (infector) to various infectees.
				// Unlike InfectSweep though, here we don't explicitly consider infectees, so take an average over whole population, weighted by age distribution.
				double AvContactRate_Infector = 0;
				// sum over "infectee" age groups to scale infectiousness of this infector.
				for (int InfecteeAge = 0; InfecteeAge < NUM_AGE_GROUPS; InfecteeAge++)
					//AvContactRate_Infector += P.PropAgeGroup[0][InfecteeAge] * P.WAIFW_Matrix_SpatialOnly[InfecteeAge][HOST_AGE_GROUP(Person)]; // use index 0 for admin unit if doing whole country.
					AvContactRate_Infector += P.PropAgeGroup[Mcells[Hosts[Person].mcell].adunit][InfecteeAge] * P.WAIFW_Matrix_SpatialOnly[InfecteeAge][HOST_AGE_GROUP(Person)];
				// scale spatial infectiousness by weighted average.
				Spatial_Infectiousness *= AvContactRate_Infector; 
			}
			int NumDaysInfectiousNotSymptomatic = (int)(LatentToSympDelay / P.ModelTimeStep);
			int InfectiousDay;
			/// Add to spatial infections from all days where latent but not symptomatic
			for (InfectiousDay = 0; InfectiousDay < NumDaysInfectiousNotSymptomatic; InfectiousDay++) SpatialInfections += Spatial_Infectiousness * P.infectiousness[InfectiousDay];
			Spatial_Infectiousness *= ((Hosts[Person].infectiousness < 0) ? P.SymptSpatialContactRate : 1);
			/// Add to spatial infections when symptomatic (note do not initialize InfectiousDay again.)
			for (; InfectiousDay < (int)Hosts[Person].recovery_or_death_time; InfectiousDay++) SpatialInfections += Spatial_Infectiousness * P.infectiousness[InfectiousDay];
		}
	}
	// Divide total spatial infections by PopSize to get Spatial R0. 
	double Spatial_R0 = SpatialInfections  / (double)P.PopSize;
	// Divide total household infections by summed household denominators to get household secondary attack rate
	Files::xfprintf_stderr("Household SAR = %lg\n", HH_Infections / HH_SAR_Denom);
	// Divide total household infections by PopSize to get household R0
	P.R0household = HH_Infections / ((double)P.PopSize);
	Files::xfprintf_stderr("Household R0 = %lg\n", P.R0household);

	// ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** // ** 
	// ** // ** Place Infections
	if (P.DoPlaces)
		for (int PlaceType = 0; PlaceType < P.NumPlaceTypes; PlaceType++)
			if (PlaceType != P.HotelPlaceType)
			{
				double PreviousTotalPlaceInfections = PlaceInfections;
#pragma omp parallel for private(ProbSurvive,LatentToSympDelay,Place_Infectiousness,NumPeopleInPlaceGroup,ProbSurviveNonCareHome) schedule(static,1000) reduction(+:PlaceInfections) default(none) shared(P, Hosts, Places, PlaceType)
				for (int Person = 0; Person < P.PopSize; Person++)
				{
					int PlaceNum = Hosts[Person].PlaceLinks[PlaceType];
					if (PlaceNum >= 0) //// i.e. if person has a link to a particular Place of this PlaceType.
					{
						LatentToSympDelay = (P.LatentToSymptDelay > Hosts[Person].recovery_or_death_time * P.ModelTimeStep) ? Hosts[Person].recovery_or_death_time * P.ModelTimeStep : P.LatentToSymptDelay;
						Place_Infectiousness = fabs(Hosts[Person].infectiousness) * P.ModelTimeStep * P.PlaceTypeTrans[PlaceType];
						double PlaceInf_Scaled = Place_Infectiousness / P.PlaceTypeGroupSizeParam1[PlaceType];
						ProbSurvive = 1.0;
						int NumDaysInfectiousNotSymptomatic = (int)(LatentToSympDelay / P.ModelTimeStep);
						int InfectiousDay; 
						for (InfectiousDay = 0; InfectiousDay < NumDaysInfectiousNotSymptomatic; InfectiousDay++)
						{
							double ProbSurviveToday = 1.0 - PlaceInf_Scaled * P.infectiousness[InfectiousDay];
							ProbSurvive *= ((ProbSurviveToday < 0) ? 0 : ProbSurviveToday);
						}
						NumPeopleInPlaceGroup = ((double)(_I64(Places[PlaceType][PlaceNum].group_size[Hosts[Person].PlaceGroupLinks[PlaceType]]) - 1));
						PlaceInf_Scaled *= (((Hosts[Person].infectiousness < 0) && (!Hosts[Person].care_home_resident)) ? // if person symptomatic and not a care home resident
							(P.SymptPlaceTypeContactRate[PlaceType] * (1 - P.SymptPlaceTypeWithdrawalProp[PlaceType])) : 1);
						for (; InfectiousDay < (int)Hosts[Person].recovery_or_death_time; InfectiousDay++)
						{
							double ProbSurviveToday = 1.0 - PlaceInf_Scaled * P.infectiousness[InfectiousDay];
							ProbSurvive *= ((ProbSurviveToday < 0) ? 0 : ProbSurviveToday);
						}

						ProbSurviveNonCareHome = ProbSurvive;
						PlaceInf_Scaled = P.PlaceTypePropBetweenGroupLinks[PlaceType] * Place_Infectiousness / ((double)Places[PlaceType][PlaceNum].n);

						// use group structure to model multiple care homes with shared staff - in which case residents of one "group" don't mix with those in another, only staff do.
						// calculation uses average proportion of care home "members" who are residents.
						if (Hosts[Person].care_home_resident)
							PlaceInf_Scaled *= (1.0 - P.CareHomePropResidents) + P.CareHomePropResidents * (P.CareHomeWorkerGroupScaling * (((double)Places[PlaceType][PlaceNum].n - 1) - NumPeopleInPlaceGroup) + NumPeopleInPlaceGroup) / ((double)Places[PlaceType][PlaceNum].n - 1);
						ProbSurvive = 1.0;
						for (InfectiousDay = 0; InfectiousDay < NumDaysInfectiousNotSymptomatic; InfectiousDay++)
						{
							double ProbSurviveToday = 1.0 - PlaceInf_Scaled * P.infectiousness[InfectiousDay];
							ProbSurvive *= ((ProbSurviveToday < 0) ? 0 : ProbSurviveToday);
						}
						PlaceInf_Scaled *= (((Hosts[Person].infectiousness < 0) && (!Hosts[Person].care_home_resident)) ? (P.SymptPlaceTypeContactRate[PlaceType] * (1 - P.SymptPlaceTypeWithdrawalProp[PlaceType])) : 1);
						for (; InfectiousDay < (int)Hosts[Person].recovery_or_death_time; InfectiousDay++)
						{
							double ProbSurviveToday = 1.0 - PlaceInf_Scaled * P.infectiousness[InfectiousDay];
							ProbSurvive *= ((ProbSurviveToday < 0) ? 0 : ProbSurviveToday);
						}
						// add to PlaceInfections. Weighted sum of PlaceGroup and non-place group infectins within that place.
						PlaceInfections += (1 - ProbSurviveNonCareHome * ProbSurvive) * NumPeopleInPlaceGroup + (1 - ProbSurvive) * (((double)(_I64(Places[PlaceType][PlaceNum].n) - 1)) - NumPeopleInPlaceGroup);
					}
				}
				Files::xfprintf_stderr("%lg  ", (PlaceInfections - PreviousTotalPlaceInfections) / ((double)P.PopSize));
			}
	double recovery_time_days = 0;
	double recovery_time_timesteps = 0;
#pragma omp parallel for schedule(static,500) reduction(+:recovery_time_days,recovery_time_timesteps) default(none) shared(P, Hosts)
	for (int Person = 0; Person < P.PopSize; Person++)
	{
		recovery_time_days += Hosts[Person].recovery_or_death_time * P.ModelTimeStep;
		recovery_time_timesteps += Hosts[Person].recovery_or_death_time;
		Hosts[Person].recovery_or_death_time = 0; // reset everybody's recovery_or_death_time
	}

	// Divide total number of place infections by PopSize to get "place" R0. 
	P.R0places = PlaceInfections / ((double)P.PopSize);
	recovery_time_days /= ((double)P.PopSize);
	recovery_time_timesteps /= ((double)P.PopSize);
	Files::xfprintf_stderr("\nR0 for places = %lg\n", P.R0places);
	if (!P.FixLocalBeta)
	{
		if (P.DoSI)
			P.LocalBeta = (P.R0 / Spatial_R0 - P.R0household - P.R0places);
		else
			P.LocalBeta = (P.R0 - P.R0household - P.R0places) / Spatial_R0;
		if (P.LocalBeta < 0) P.LocalBeta = 0;
		Files::xfprintf_stderr("Set spatial beta to %lg\n", P.LocalBeta);
	}
	P.R0spatial = Spatial_R0 * P.LocalBeta;
	P.R0		= P.R0household + P.R0places + P.R0spatial;
	Files::xfprintf_stderr("R0 for random spatial = %lg\nOverall R0 = %lg\n", P.R0spatial, P.R0);
	Files::xfprintf_stderr("Mean infectious period (sampled) = %lg (%lg)\n", recovery_time_days, recovery_time_timesteps);
}

void SetupPopulation(std::string const& density_file, std::string const& out_density_file, std::string const& school_file, std::string const& reg_demog_file)
{
	int j, l, m, i2, j2, last_i, mr, ad, country;
	unsigned int rn, rn2;
	double t, s, x, y, xh, yh, maxd, CumAgeDist[NUM_AGE_GROUPS + 1];
	char buf[4096], *col;
	const char delimiters[] = " \t,";
	FILE* dat = NULL, *dat2;
	BinFile rec;
	double *mcell_dens;
	int *mcell_adunits, *mcell_num;

	// allocate memory
	Cells			= (Cell*)		Memory::xcalloc(P.NumCells		, sizeof(Cell));
	Mcells			= (Microcell*)	Memory::xcalloc(P.NumMicrocells	, sizeof(Microcell));
	mcell_num		= (int*)		Memory::xcalloc(P.NumMicrocells	, sizeof(int));
	mcell_dens		= (double*)		Memory::xcalloc(P.NumMicrocells	, sizeof(double));
	mcell_country	= std::vector<uint16_t>(P.NumMicrocells, 1);
	mcell_adunits	= (int*)		Memory::xcalloc(P.NumMicrocells	, sizeof(int));

	for (int mcell = 0; mcell < P.NumMicrocells; mcell++)
	{
		Mcells[mcell].n = 0;
		mcell_adunits[mcell] = -1;
		mcell_dens[mcell] = 0;
		mcell_num[mcell] = 0;
	}
	if (P.DoAdUnits)
		for (int i = 0; i < MAX_ADUNITS; i++)
			P.PopByAdunit[i][0] = P.PopByAdunit[i][1] = 0;

	if (!density_file.empty())
	{
		if (!P.DoAdunitBoundaries) P.NumAdunits = 0;
		//		dat2 = Files::xfopen("EnvTest.txt","w");
		Files::xfprintf_stderr("Density file contains %i datapoints.\n", (int)P.BinFileLen);
		for (rn = rn2 = mr = 0; rn < P.BinFileLen; rn++)
		{
			int k;
			x = BF[rn].x; y = BF[rn].y; t = BF[rn].pop; country = BF[rn].cnt; j2 = BF[rn].ad;
			rec = BF[rn];
			if (P.DoAdUnits)
			{
				m = (j2 % P.AdunitLevel1Mask) / P.AdunitLevel1Divisor;
				if (P.DoAdunitBoundaries)
				{
					if (P.AdunitLevel1Lookup[m] >= 0)
					{
						if (j2 / P.AdunitLevel1Mask == AdUnits[P.AdunitLevel1Lookup[m]].id / P.AdunitLevel1Mask)
						{
							k = 1;
							AdUnits[P.AdunitLevel1Lookup[m]].cnt_id = country;
						}
						else	k = 0;
					}
					else	k = 0;
				}
				else
				{
					k = 1;
					if (P.AdunitLevel1Lookup[m] < 0)
					{
						P.AdunitLevel1Lookup[m] = P.NumAdunits;
						AdUnits[P.NumAdunits].id = j2;
						AdUnits[P.NumAdunits].cnt_id = country;
						P.NumAdunits++;
						if (P.NumAdunits >= MAX_ADUNITS) ERR_CRITICAL("Total number of administrative units exceeds MAX_ADUNITS\n");
					}
					else AdUnits[P.AdunitLevel1Lookup[m]].cnt_id = country;
				}
			}
			else k = 1;

			if ((k) && P.SpatialBoundingBox.inside(CovidSim::Geometry::Vector2d(x, y)))
			{
				j = (int)floor((x - P.SpatialBoundingBox.bottom_left().x) / P.in_microcells_.width  + 0.1);
				k = (int)floor((y - P.SpatialBoundingBox.bottom_left().y) / P.in_microcells_.height + 0.1);
				l = j * P.total_microcells_high_ + k;
				if (l < P.NumMicrocells)
				{
					mr++;
					mcell_dens[l] += t;
					mcell_country[l] = country;
					//Files::xfprintf_stderr("mcell %i, country %i, pop %lg\n",l,country,t);
					mcell_num[l]++;
					if (P.DoAdUnits)
					{
						mcell_adunits[l] = P.AdunitLevel1Lookup[m];
						if (mcell_adunits[l] < 0) Files::xfprintf_stderr("Microcell %i has adunits<0\n", l);
						P.PopByAdunit[P.AdunitLevel1Lookup[m]][0] += t;
					}
					else
						mcell_adunits[l] = 0;
					if (!out_density_file.empty() && (P.DoBin) && (mcell_adunits[l] >= 0))
					{
						if (rn2 < rn) BF[rn2] = rec;
						rn2++;
					}
				}
			}
		}
		//		Files::xfclose(dat2);
		Files::xfprintf_stderr("%i valid microcells read from density file.\n", mr);
		if (!out_density_file.empty() && (P.DoBin)) P.BinFileLen = rn2;
		if (P.DoBin == 0)
		{
			if (!out_density_file.empty())
			{
				Memory::xfree(BinFileBuf);
				P.DoBin = 1;
				P.BinFileLen = 0;
				for (l = 0; l < P.NumMicrocells; l++)
					if (mcell_adunits[l] >= 0) P.BinFileLen++;
				BinFileBuf = (void*)Memory::xcalloc(P.BinFileLen, sizeof(BinFile));
				BF = (BinFile*)BinFileBuf;
				Files::xfprintf_stderr("Binary density file should contain %i microcells.\n", (int)P.BinFileLen);
				rn = 0;
				for (l = 0; l < P.NumMicrocells; l++)
					if (mcell_adunits[l] >= 0)
					{
						BF[rn].x = (double)(P.in_microcells_.width * (((double)(l / P.total_microcells_high_)) + 0.5)) + P.SpatialBoundingBox.bottom_left().x; //x
						BF[rn].y = (double)(P.in_microcells_.height * (((double)(l % P.total_microcells_high_)) + 0.5)) + P.SpatialBoundingBox.bottom_left().y; //y
						BF[rn].ad = (P.DoAdUnits) ? (AdUnits[mcell_adunits[l]].id) : 0;
						BF[rn].pop = mcell_dens[l];
						BF[rn].cnt = mcell_country[l];
						rn++;
					}
			}
		}

		if (!out_density_file.empty())
		{
			dat2 = Files::xfopen(out_density_file.c_str(), "wb");
			rn = 0xf0f0f0f0;
			Files::fwrite_big((void*)& rn, sizeof(unsigned int), 1, dat2);
			Files::xfprintf_stderr("Saving population density file with NC=%i...\n", (int)P.BinFileLen);
			Files::fwrite_big((void*) & (P.BinFileLen), sizeof(unsigned int), 1, dat2);
			Files::fwrite_big(BinFileBuf, sizeof(BinFile), (size_t)P.BinFileLen, dat2);
			Files::xfclose(dat2);
		}
		Memory::xfree(BinFileBuf);
		Files::xfprintf_stderr("Population files read.\n");
		maxd = 0;
		for (int i = 0; i < P.NumMicrocells; i++)
		{
			if (mcell_num[i] > 0)
			{
				mcell_dens[i] /= ((double)mcell_num[i]);
				if (P.DoAdUnits)
					Mcells[i].adunit = mcell_adunits[i];
				else
					Mcells[i].adunit = 0;
			}
			else
				Mcells[i].adunit = -1;
			maxd += mcell_dens[i];
		}
	}
	else
	{
		for (int i = 0; i < P.NumMicrocells; i++)
			mcell_dens[i] = 1.0;
		maxd = ((double)P.NumMicrocells);
	}
	if (!P.DoAdUnits) P.NumAdunits = 1;
	if ((P.DoAdUnits) && !reg_demog_file.empty())
	{
		State.InvAgeDist = (int**)Memory::xcalloc(P.NumAdunits, sizeof(int*));
		for (int i = 0; i < P.NumAdunits; i++)
			State.InvAgeDist[i] = (int*)Memory::xcalloc(1000, sizeof(int));
		dat = Files::xfopen(reg_demog_file.c_str(), "rb");
		for (int k = 0; k < P.NumAdunits; k++)
		{
			for (int i = 0; i < NUM_AGE_GROUPS; i++)
				P.PropAgeGroup[k][i] = 0;
			for (int i = 0; i < MAX_HOUSEHOLD_SIZE; i++)
				P.HouseholdSizeDistrib[k][i] = 0;
			P.PopByAdunit[k][1] = 0;
		}
		while (!feof(dat))
		{
			fgets(buf, 2047, dat);
			col = strtok(buf, delimiters);
			Files::xsscanf(col, 1, "%i", &l);
			m = (l % P.AdunitLevel1Mask) / P.AdunitLevel1Divisor;
			int k = P.AdunitLevel1Lookup[m];
			if (k >= 0)
				if (l / P.AdunitLevel1Mask == AdUnits[k].id / P.AdunitLevel1Mask)
				{
					col = strtok(NULL, delimiters);
					Files::xsscanf(col, 1, "%lg", &x);
					P.PopByAdunit[k][1] += x;
					t = 0;
					for (int i = 0; i < NUM_AGE_GROUPS; i++)
					{
						col = strtok(NULL, delimiters);
						Files::xsscanf(col, 1, "%lg", &s);
						P.PropAgeGroup[k][i] += s;
					}
					col = strtok(NULL, delimiters);
					if (P.DoHouseholds)
					{
						Files::xsscanf(col, 1, "%lg", &y);
						for (int i = 0; i < MAX_HOUSEHOLD_SIZE; i++)
						{
							col = strtok(NULL, delimiters);
							Files::xsscanf(col, 1, "%lg", &s);
							P.HouseholdSizeDistrib[k][i] += y * s;
						}
					}
				}
		}
		Files::xfclose(dat);
		for (int k = 0; k < P.NumAdunits; k++)
		{
			t = 0;
			for (int i = 0; i < NUM_AGE_GROUPS; i++)
				t += P.PropAgeGroup[k][i];
			CumAgeDist[0] = 0;
			for (int i = 1; i <= NUM_AGE_GROUPS; i++)
			{
				P.PropAgeGroup[k][i - 1] /= t;
				CumAgeDist[i] = CumAgeDist[i - 1] + P.PropAgeGroup[k][i - 1];
			}
			for (int i = j = 0; i < 1000; i++)
			{
				t = ((double)i) / 1000;
				while (t >= CumAgeDist[j + 1]) j++;
				t = AGE_GROUP_WIDTH * (((double)j) + (t - CumAgeDist[j]) / (CumAgeDist[j + 1] - CumAgeDist[j]));
				State.InvAgeDist[k][i] = (int)t;
			}
			State.InvAgeDist[k][1000 - 1] = NUM_AGE_GROUPS * AGE_GROUP_WIDTH - 1;
			if (P.DoHouseholds)
			{
				t = 0;
				for (int i = 0; i < MAX_HOUSEHOLD_SIZE; i++)
					t += P.HouseholdSizeDistrib[k][i];
				P.HouseholdSizeDistrib[k][0] /= t;
				for (int i = 1; i < MAX_HOUSEHOLD_SIZE - 1; i++)
					P.HouseholdSizeDistrib[k][i] = P.HouseholdSizeDistrib[k][i] / t + P.HouseholdSizeDistrib[k][i - 1];
				P.HouseholdSizeDistrib[k][MAX_HOUSEHOLD_SIZE - 1] = 1.0;
			}
			else
			{
				for (int i = 0; i < MAX_HOUSEHOLD_SIZE - 1; i++)
					P.HouseholdSizeDistrib[k][i] = 1.0;
			}
		}
	}
	else
	{
		State.InvAgeDist = (int**)Memory::xcalloc(1, sizeof(int*));
		State.InvAgeDist[0] = (int*)Memory::xcalloc(1000, sizeof(int));
		CumAgeDist[0] = 0;
		for (int i = 1; i <= NUM_AGE_GROUPS; i++)
			CumAgeDist[i] = CumAgeDist[i - 1] + P.PropAgeGroup[0][i - 1];
		for (int i = j = 0; i < 1000; i++)
		{
			t = ((double)i) / 1000;
			if (t >= CumAgeDist[j + 1]) j++;
			t = AGE_GROUP_WIDTH * (((double)j) + (t - CumAgeDist[j]) / (CumAgeDist[j + 1] - CumAgeDist[j]));
			State.InvAgeDist[0][i] = (int)t;
		}
		State.InvAgeDist[0][1000 - 1] = NUM_AGE_GROUPS * AGE_GROUP_WIDTH - 1;
	}
	if (P.DoAdUnits)
		for (int i = 0; i < P.NumAdunits; i++) AdUnits[i].n = 0;
	if ((P.DoAdUnits) && !reg_demog_file.empty() && (P.DoCorrectAdunitPop))
	{
		for (int i = 0; i < P.NumAdunits; i++)
			Files::xfprintf_stderr("%i\t%i\t%lg\t%lg\n", i, (AdUnits[i].id % P.AdunitLevel1Mask) / P.AdunitLevel1Divisor, P.PropAgeGroup[i][0], P.HouseholdSizeDistrib[i][0]);
		maxd = 0;
		for (int i = 0; i < P.NumMicrocells; i++)
		{
			if (mcell_num[i] > 0)
			{
				if (mcell_adunits[i] < 0) ERR_CRITICAL_FMT("Cell %i has adunits < 0 (indexing PopByAdunit)\n", i);
				mcell_dens[i] *= P.PopByAdunit[mcell_adunits[i]][1] / (1e-10 + P.PopByAdunit[mcell_adunits[i]][0]);
			}
			maxd += mcell_dens[i];
		}
		t = 0;
		for (int i = 0; i < P.NumAdunits; i++)
			t += P.PopByAdunit[i][1];
		int i = P.PopSize;
		P.PopSize = (int)t;
		Files::xfprintf_stderr("Population size reset from %i to %i\n", i, P.PopSize);
	}
	t = 1.0;
	for (int i = m = 0; i < (P.NumMicrocells - 1); i++)
	{
		s = mcell_dens[i] / maxd / t;
		if (s > 1.0) s = 1.0;
		m += (Mcells[i].n = (int)ignbin_mt((int32_t)(P.PopSize - m), s, 0));
		t -= mcell_dens[i] / maxd;
		if (Mcells[i].n > 0)
		{
			P.NumPopulatedMicrocells++;
			if (mcell_adunits[i] < 0) ERR_CRITICAL_FMT("Cell %i has adunits < 0 (indexing AdUnits)\n", i);
			AdUnits[mcell_adunits[i]].n += Mcells[i].n;
		}
	}
	Mcells[P.NumMicrocells - 1].n = P.PopSize - m;
	if (Mcells[P.NumMicrocells - 1].n > 0)
	{
		P.NumPopulatedMicrocells++;
		AdUnits[mcell_adunits[P.NumMicrocells - 1]].n += Mcells[P.NumMicrocells - 1].n;
	}

	Memory::xfree(mcell_dens);
	Memory::xfree(mcell_num);
	Memory::xfree(mcell_adunits);
	t = 0.0;

	McellLookup = (Microcell **)Memory::xcalloc(P.NumPopulatedMicrocells, sizeof(Microcell*));
	State.CellMemberArray = (int*)Memory::xcalloc(P.PopSize, sizeof(int));
	P.NumPopulatedCells = 0;
	for (int i = i2 = j2 = 0; i < P.NumCells; i++)
	{
		Cells[i].n = 0;
		int k = (i / P.nch) * P.NMCL * P.total_microcells_high_ + (i % P.nch) * P.NMCL;
		Cells[i].members = State.CellMemberArray + j2;
		for (l = 0; l < P.NMCL; l++)
			for (m = 0; m < P.NMCL; m++)
			{
				j = k + m + l * P.total_microcells_high_;
				if (Mcells[j].n > 0)
				{
					Mcells[j].members = State.CellMemberArray + j2;
					//Mcells[j].members=(int *)Memory::xcalloc(Mcells[j].n,sizeof(int)); //replaced line above with this to ensure members don't get mixed across microcells
					McellLookup[i2++] = Mcells + j;
					Cells[i].n += Mcells[j].n;
					j2 += Mcells[j].n;
				}
			}
		if (Cells[i].n > 0) P.NumPopulatedCells++;
	}
	Files::xfprintf_stderr("Number of hosts assigned = %i\n", j2);
	if (!P.DoAdUnits) P.AdunitLevel1Lookup[0] = 0;
	Files::xfprintf_stderr("Number of cells with non-zero population = %i\n", P.NumPopulatedCells);
	Files::xfprintf_stderr("Number of microcells with non-zero population = %i\n", P.NumPopulatedMicrocells);

	CellLookup = (Cell **)Memory::xcalloc(P.NumPopulatedCells, sizeof(Cell*));
	State.CellSuscMemberArray = (int*)Memory::xcalloc(P.PopSize, sizeof(int));
	int susceptibleAccumulator = 0;
	i2 = 0;
	for (j = 0; j < P.NumCells; j++)
		if (Cells[j].n > 0)
		{
			CellLookup[i2++] = Cells + j;
			Cells[j].susceptible = State.CellSuscMemberArray + susceptibleAccumulator;
			susceptibleAccumulator += Cells[j].n;
		}
	if (i2 > P.NumPopulatedCells) Files::xfprintf_stderr("######## Over-run on CellLookup array NCP=%i i2=%i ###########\n", P.NumPopulatedCells, i2);
	i2 = 0;

	Hosts = (Person*)Memory::xcalloc(P.PopSize, sizeof(Person));
	HostsQuarantine = std::vector<PersonQuarantine>(P.PopSize, PersonQuarantine());
	Files::xfprintf_stderr("sizeof(Person)=%i\n", (int) sizeof(Person));
	for (int i = 0; i < P.NumPopulatedCells; i++)
	{
		Cell *c = CellLookup[i];
		if (c->n > 0)
		{
			c->InvCDF = (int*)Memory::xcalloc(1025, sizeof(int));
			c->max_trans = (float*)Memory::xcalloc(P.NumPopulatedCells, sizeof(float));
			c->cum_trans = (float*)Memory::xcalloc(P.NumPopulatedCells, sizeof(float));
		}
	}
	for (int i = 0; i < P.NumCells; i++)
	{
		Cells[i].cumTC = 0;
		for (j = 0; j < Cells[i].n; j++) Cells[i].members[j] = -1;
	}
	Files::xfprintf_stderr("Cells assigned\n");
	for (int i = 0; i <= MAX_HOUSEHOLD_SIZE; i++) denom_household[i] = 0;
	P.NumHouseholds = 0;
	int numberOfPeople = 0;
	int MCell = 0;
	for (int PopMCellIndex = 0; PopMCellIndex < P.NumPopulatedMicrocells; PopMCellIndex++)
	{
		MCell = (int)(McellLookup[PopMCellIndex] - Mcells);
		l = ((MCell / P.total_microcells_high_) / P.NMCL) * P.nch + ((MCell % P.total_microcells_high_) / P.NMCL);
		ad = (!reg_demog_file.empty() && (P.DoAdUnits)) ? Mcells[MCell].adunit : 0;
		for (int k = 0; k < Mcells[MCell].n;)
		{
			m = 1;
			if (P.DoHouseholds)
			{
				s = ranf_mt(0);
				while ((s > P.HouseholdSizeDistrib[ad][m - 1]) && (k + m < Mcells[MCell].n) && (m < MAX_HOUSEHOLD_SIZE)) m++;
			}
			denom_household[m]++;
			for (i2 = 0; i2 < m; i2++)
			{
				//				Files::xfprintf_stderr("%i ",i+i2);
				Hosts[numberOfPeople + i2].listpos = m; //used temporarily to store household size
				Mcells[MCell].members[k + i2] = numberOfPeople + i2;
				Cells[l].susceptible[Cells[l].cumTC] = numberOfPeople + i2;
				Cells[l].members[Cells[l].cumTC++] = numberOfPeople + i2;
				Hosts[numberOfPeople + i2].pcell = l;
				Hosts[numberOfPeople + i2].mcell = MCell;
				Hosts[numberOfPeople + i2].hh = P.NumHouseholds;
			}
			P.NumHouseholds++;
			numberOfPeople += m;
			k += m;
		}
	}
	Households = (Household*)Memory::xcalloc(P.NumHouseholds, sizeof(Household));
	for (j = 0; j < NUM_AGE_GROUPS; j++) AgeDist[j] = AgeDist2[j] = 0;
	if (P.DoHouseholds) Files::xfprintf_stderr("Household sizes assigned to %i people\n", numberOfPeople);

	FILE* stderr_shared = stderr;
#pragma omp parallel for private(j2,j,x,y,xh,yh,i2,m) schedule(static,1) default(none) \
		shared(P, Households, Hosts, Mcells, McellLookup, AdUnits, reg_demog_file, stderr_shared)
	for (int tn = 0; tn < P.NumThreads; tn++)
		for (j2 = tn; j2 < P.NumPopulatedMicrocells; j2 += P.NumThreads)
		{
			j = (int)(McellLookup[j2] - Mcells);
			x = (double)(j / P.total_microcells_high_);
			y = (double)(j % P.total_microcells_high_);
			int i = Mcells[j].members[0];
			if (j % 100 == 0)
				Files::xfprintf(stderr_shared, "%i=%i (%i %i)            \r", j, Mcells[j].n, Mcells[j].adunit, (AdUnits[Mcells[j].adunit].id % P.AdunitLevel1Mask) / P.AdunitLevel1Divisor);
			for (int k = 0; k < Mcells[j].n;)
			{
				m = Hosts[i].listpos;
				xh = P.in_microcells_.width * (ranf_mt(tn) + x);
				yh = P.in_microcells_.height * (ranf_mt(tn) + y);
				AssignHouseholdAges(m, i, tn, !reg_demog_file.empty());
				for (i2 = 0; i2 < m; i2++) Hosts[i + i2].listpos = 0;
				if (P.DoHouseholds)
				{
					for (i2 = 0; i2 < m; i2++) {
						Hosts[i + i2].set_susceptible(); //added this so that infection status is set to zero and household r0 is correctly calculated
					}
				}
				Households[Hosts[i].hh].FirstPerson = i;
				Households[Hosts[i].hh].nh = m;
				Households[Hosts[i].hh].nhr = m;
				Households[Hosts[i].hh].loc.x = (float)xh;
				Households[Hosts[i].hh].loc.y = (float)yh;
				i += m;
				k += m;
			}
		}
	if (P.DoCorrectAgeDist)
	{
		double** AgeDistAd, ** AgeDistCorrF, ** AgeDistCorrB;
		AgeDistAd = (double**)Memory::xcalloc(MAX_ADUNITS, sizeof(double*));
		AgeDistCorrF = (double**)Memory::xcalloc(MAX_ADUNITS, sizeof(double*));
		AgeDistCorrB = (double**)Memory::xcalloc(MAX_ADUNITS, sizeof(double*));
		for (int i = 0; i < P.NumAdunits; i++)
		{
			AgeDistAd[i] = (double*)Memory::xcalloc((NUM_AGE_GROUPS + 1), sizeof(double));
			AgeDistCorrF[i] = (double*)Memory::xcalloc((NUM_AGE_GROUPS + 1), sizeof(double));
			AgeDistCorrB[i] = (double*)Memory::xcalloc((NUM_AGE_GROUPS + 1), sizeof(double));
		}

		// compute AgeDistAd[i][j] = total number of people in adunit i, age group j
		for (int i = 0; i < P.NumAdunits; i++)
			for (j = 0; j < NUM_AGE_GROUPS; j++)
				AgeDistAd[i][j] = 0;
		for (int i = 0; i < P.PopSize; i++)
		{
			int k = !reg_demog_file.empty() ? Mcells[Hosts[i].mcell].adunit : 0;
			AgeDistAd[k][HOST_AGE_GROUP(i)]++;
		}
		// normalize AgeDistAd[i][j], so it's the proportion of people in adunit i that are in age group j
		int k = !reg_demog_file.empty() ? P.NumAdunits : 1;
		for (int i = 0; i < k; i++)
		{
			s = 0.0;
			for (j = 0; j < NUM_AGE_GROUPS; j++)
				s += AgeDistAd[i][j];
			for (j = 0; j < NUM_AGE_GROUPS; j++)
				AgeDistAd[i][j] /= s;
		}
		// determine adjustments to be made to match age data in parameters
		for (int i = 0; i < k; i++)
		{
			s = t = 0;
			AgeDistCorrB[i][0] = 0;
			for (j = 0; j < NUM_AGE_GROUPS; j++)
			{
				// compute s = the proportion of people that need removing from adunit i, age group j to match age data in parameters
				s = t + AgeDistAd[i][j] - P.PropAgeGroup[i][j] - AgeDistCorrB[i][j];
				if (s > 0)
				{
					t = AgeDistCorrF[i][j] = s; // people to push up into next age group
					AgeDistCorrB[i][j + 1] = 0;
				}
				else
				{
					t = AgeDistCorrF[i][j] = 0;
					AgeDistCorrB[i][j + 1] = fabs(s); // people to pull down from next age group
				}
				AgeDistCorrF[i][j] /= AgeDistAd[i][j]; // convert from proportion of people in the adunit to proportion of people in the adunit and age group
				AgeDistCorrB[i][j] /= AgeDistAd[i][j];
			}
			// output problematic adjustments (these should be 0.0f)
			//Files::xfprintf_stderr("AgeDistCorrB[%i][0] = %f\n", i, AgeDistCorrB[i][0]); // push down from youngest age group
			//Files::xfprintf_stderr("AgeDistCorrF[%i][NUM_AGE_GROUPS - 1] = %f\n", i, AgeDistCorrF[i][NUM_AGE_GROUPS - 1]); // push up from oldest age group
			//Files::xfprintf_stderr("AgeDistCorrB[%i][NUM_AGE_GROUPS] = %f\n", i, AgeDistCorrB[i][NUM_AGE_GROUPS]); // push down from oldest age group + 1
		}

		// make age adjustments to population
#pragma omp parallel for private(j,m,s) schedule(static,1) default(none) \
			shared(P, Hosts, AgeDistCorrF, AgeDistCorrB, Mcells, reg_demog_file)
		for (int tn = 0; tn < P.NumThreads; tn++)
			for (int i = tn; i < P.PopSize; i += P.NumThreads)
			{
				m = !reg_demog_file.empty() ? Mcells[Hosts[i].mcell].adunit : 0;
				j = HOST_AGE_GROUP(i);
				s = ranf_mt(tn);
				// probabilistic age adjustment by one age category (5 years)
				if (s < AgeDistCorrF[m][j])
					Hosts[i].age += 5;
				else if (s < AgeDistCorrF[m][j] + AgeDistCorrB[m][j])
					Hosts[i].age -= 5;
			}
		for (int i = 0; i < P.NumAdunits; i++)
		{
			Memory::xfree(AgeDistAd[i]);
			Memory::xfree(AgeDistCorrF[i]);
			Memory::xfree(AgeDistCorrB[i]);
		}
		Memory::xfree(AgeDistAd);
		Memory::xfree(AgeDistCorrF);
		Memory::xfree(AgeDistCorrB);
	}
	for (int i = 0; i < P.PopSize; i++)
	{
		if (Hosts[i].age >= NUM_AGE_GROUPS * AGE_GROUP_WIDTH)
		{
			ERR_CRITICAL_FMT("Person %i has unexpected age %i\n", i, Hosts[i].age);
		}
		AgeDist[HOST_AGE_GROUP(i)]++;
	}
	Files::xfprintf_stderr("Ages/households assigned\n");

	if (!P.DoRandomInitialInfectionLoc)
	{
		int k = (int)(P.LocationInitialInfection[0][0] / P.in_microcells_.width);
		l = (int)(P.LocationInitialInfection[0][1] / P.in_microcells_.height);
		j = k * P.total_microcells_high_ + l;

		double rand_r = 0.0; //added these variables so that if initial infection location is empty we can search the 10km neighbourhood to find a suitable cell
		double rand_theta = 0.0;
		int counter = 0;
		if (Mcells[j].n < P.NumInitialInfections[0])
		{
			while (Mcells[j].n < P.NumInitialInfections[0] && counter < 100)
			{
				rand_r = ranf(); rand_theta = ranf();
				rand_r = 0.083 * sqrt(rand_r); rand_theta = 2 * PI * rand_theta; //rand_r is multiplied by 0.083 as this is roughly equal to 10km in decimal degrees
				k = (int)((P.LocationInitialInfection[0][0] + rand_r * cos(rand_theta)) / P.in_microcells_.width);
				l = (int)((P.LocationInitialInfection[0][1] + rand_r * sin(rand_theta)) / P.in_microcells_.height);
				j = k * P.total_microcells_high_ + l;
				counter++;
			}
			if (counter < 100)
			{
				P.LocationInitialInfection[0][0] = P.LocationInitialInfection[0][0] + rand_r * cos(rand_theta); //set LocationInitialInfection to actual one used
				P.LocationInitialInfection[0][1] = P.LocationInitialInfection[0][1] + rand_r * sin(rand_theta);
			}
		}
		if (Mcells[j].n < P.NumInitialInfections[0])
			ERR_CRITICAL("Too few people in seed microcell to start epidemic with required number of initial infectionz.\n");
	}
	Files::xfprintf_stderr("Checking cells...\n");
	maxd = ((double)P.PopSize);
	last_i = 0;
	for (int i = 0; i < P.NumMicrocells; i++)
		if (Mcells[i].n > 0) last_i = i;
	Files::xfprintf_stderr("Allocating place/age groups...\n");
	for (int k = 0; k < NUM_AGE_GROUPS * AGE_GROUP_WIDTH; k++)
	{
		for (l = 0; l < P.NumPlaceTypes; l++)
		{
			PropPlaces[k][l] = PropPlacesC[k][l] = 0.0;
			if ((k < P.PlaceTypeAgeMax[l]) && (k >= P.PlaceTypeAgeMin[l]))
				PropPlaces[k][l] += P.PlaceTypePropAgeGroup[l];
			if ((k < P.PlaceTypeAgeMax2[l]) && (k >= P.PlaceTypeAgeMin2[l]))
				PropPlaces[k][l] += P.PlaceTypePropAgeGroup2[l];
			if ((k < P.PlaceTypeAgeMax3[l]) && (k >= P.PlaceTypeAgeMin3[l]))
				PropPlaces[k][l] += P.PlaceTypePropAgeGroup3[l];
			if (l == P.HotelPlaceType)
				PropPlacesC[k][l] = ((l > 0) ? PropPlacesC[k][l - 1] : 0);
			else
				PropPlacesC[k][l] = PropPlaces[k][l] + ((l > 0) ? PropPlacesC[k][l - 1] : 0);
		}
	}
	/*
		for(l=0;l<P.NumPlaceTypes;l++)
			{
			for(k=0;k<NUM_AGE_GROUPS*AGE_GROUP_WIDTH;k++)
				Files::xfprintf_stderr("%i:%lg ",k,PropPlaces[k][l]);
			Files::xfprintf_stderr("\n");
			}
	*/
	/*	if((P.DoAdUnits)&&(P.DoAdunitDemog))
			{for(i=0;i<P.NumAdunits;i++) Memory::xfree(State.InvAgeDist[i]);}
		else
			Memory::xfree(State.InvAgeDist[0]);
		Memory::xfree(State.InvAgeDist);
	*/	P.nsp = 0;
	if (P.DoPlaces)
		Places = (Place **)Memory::xcalloc(P.NumPlaceTypes, sizeof(Place*));
	if (!school_file.empty() && (P.DoPlaces))
	{
		Files::xfprintf_stderr("Reading school file\n");
		dat = Files::xfopen(school_file.c_str(), "rb");
		Files::xfscanf(dat, 1, "%i", &P.nsp);
		for (j = 0; j < P.nsp; j++)
		{
			Files::xfscanf(dat, 2, "%i %i", &m, &(P.PlaceTypeMaxAgeRead[j]));
			Places[j] = (Place*)Memory::xcalloc(m, sizeof(Place));
			for (int i = 0; i < m; i++)
				Places[j][i].AvailByAge = (unsigned short int*)Memory::xcalloc(P.PlaceTypeMaxAgeRead[j], sizeof(unsigned short int));
			P.Nplace[j] = 0;
			for (int i = 0; i < P.NumMicrocells; i++) Mcells[i].NumPlacesByType[j] = 0;
		}
		mr = 0;
		while (!feof(dat))
		{
			Files::xfscanf(dat, 4, "%lg %lg %i %i", &x, &y, &j, &m);
			for (int i = 0; i < P.PlaceTypeMaxAgeRead[j]; i++) Files::xfscanf(dat, 1, "%hu", &(Places[j][P.Nplace[j]].AvailByAge[i]));
			Places[j][P.Nplace[j]].loc.x = (float)(x - P.SpatialBoundingBox.bottom_left().x);
			Places[j][P.Nplace[j]].loc.y = (float)(y - P.SpatialBoundingBox.bottom_left().y);
			if (P.SpatialBoundingBox.inside(CovidSim::Geometry::Vector2d(x, y)))
			{
				int i = P.nch * ((int)(Places[j][P.Nplace[j]].loc.x / P.in_cells_.width)) + ((int)(Places[j][P.Nplace[j]].loc.y / P.in_cells_.height));
				if (Cells[i].n == 0) mr++;
				Places[j][P.Nplace[j]].n = m;
				i = (int)(Places[j][P.Nplace[j]].loc.x / P.in_microcells_.width);
				int k = (int)(Places[j][P.Nplace[j]].loc.y / P.in_microcells_.height);
				j2 = i * P.total_microcells_high_ + k;
				Mcells[j2].NumPlacesByType[j]++;
				Places[j][P.Nplace[j]].mcell = j2;
				P.Nplace[j]++;
				if (P.Nplace[j] % 1000 == 0) Files::xfprintf_stderr("%i read    \r", P.Nplace[j]);
			}
		}
		Files::xfclose(dat);
		Files::xfprintf_stderr("%i schools read (%i in empty cells)      \n", P.Nplace[j], mr);
		for (int i = 0; i < P.NumMicrocells; i++)
			for (j = 0; j < P.nsp; j++)
				if (Mcells[i].NumPlacesByType[j] > 0)
				{
					Mcells[i].places[j] = (int*)Memory::xcalloc(Mcells[i].NumPlacesByType[j], sizeof(int));
					Mcells[i].NumPlacesByType[j] = 0;
				}
		for (j = 0; j < P.nsp; j++)
		{
			t = s = 0;
			for (int i = 0; i < P.PopSize; i++)
				t += PropPlaces[HOST_AGE_YEAR(i)][j];
			for (int i = 0; i < P.Nplace[j]; i++)
			{
				int k = Places[j][i].mcell;
				Mcells[k].places[j][Mcells[k].NumPlacesByType[j]++] = i;
				s += (double)Places[j][i].n;
			}
			Files::xfprintf_stderr("School type %i: capacity=%lg demand=%lg\n", j, s, t);
			t /= s;
			for (int i = 0; i < P.Nplace[j]; i++)
				Places[j][i].n = (int)ceil(((double)Places[j][i].n) * t);
		}
	}
	if (P.DoPlaces)
	{
		Files::xfprintf_stderr("Configuring places...\n");

#pragma omp parallel for private(j2,j,t,m,s,x,y,xh,yh) schedule(static,1) default(none) \
			shared(P, Hosts, Places, PropPlaces, Mcells, maxd, last_i, mcell_country, stderr_shared)
		for (int tn = 0; tn < P.NumThreads; tn++)
			for (j2 = P.nsp + tn; j2 < P.NumPlaceTypes; j2 += P.NumThreads)
			{
				t = 0;
				P.PlaceTypeMaxAgeRead[j2] = 0;
				for (int i = 0; i < P.PopSize; i++)
					t += PropPlaces[HOST_AGE_YEAR(i)][j2];
				P.Nplace[j2] = (int)ceil(t / P.PlaceTypeMeanSize[j2]);
				Files::xfprintf(stderr_shared, "[%i:%i %g] ", j2, P.Nplace[j2], t);
				Places[j2] = (Place*)Memory::xcalloc(P.Nplace[j2], sizeof(Place));
				t = 1.0;
				int k;
				for (int i = m = k = 0; i < P.NumMicrocells; i++)
				{
					s = ((double) Mcells[i].n) / maxd / t;
					if (s > 1.0) s = 1.0;
					if (i == last_i)
						m += (Mcells[last_i].NumPlacesByType[j2] = P.Nplace[j2] - m);
					else
						m += (Mcells[i].NumPlacesByType[j2] = (int)ignbin_mt((int32_t)(P.Nplace[j2] - m), s, tn));
					t -= ((double)Mcells[i].n) / maxd;
					if (Mcells[i].NumPlacesByType[j2] > 0)
					{
						Mcells[i].places[j2] = (int*)Memory::xcalloc(Mcells[i].NumPlacesByType[j2], sizeof(int));
						x = (double)(i / P.total_microcells_high_);
						y = (double)(i % P.total_microcells_high_);
						for (j = 0; j < Mcells[i].NumPlacesByType[j2]; j++)
						{
							xh = P.in_microcells_.width * (ranf_mt(tn) + x);
							yh = P.in_microcells_.height * (ranf_mt(tn) + y);
							Places[j2][k].loc.x = (float)xh;
							Places[j2][k].loc.y = (float)yh;
							Places[j2][k].n = 0;
							Places[j2][k].mcell = i;
							Places[j2][k].country = mcell_country[i];
							Mcells[i].places[j2][j] = k;
							k++;
						}
					}
				}
			}
		for (int k = 0; k < NUM_AGE_GROUPS * AGE_GROUP_WIDTH; k++)
			for (l = 1; l < P.NumPlaceTypes; l++)
				if (l != P.HotelPlaceType)
				{
					if (PropPlacesC[k][l - 1] < 1)
						PropPlaces[k][l] /= (1 - PropPlacesC[k][l - 1]);
					else if (PropPlaces[k][l] != 0)
						PropPlaces[k][l] = 1.0;
				}
/*		for (j2 = 0; j2 < P.NumPlaceTypes; j2++)
			for (i =0; i < P.NumMicrocells; i++)
				if ((Mcells[i].NumPlacesByType[j2]>0) && (Mcells[i].n == 0))
					Files::xfprintf_stderr("\n##~ %i %i %i \n", i, j2, Mcells[i].NumPlacesByType[j2]);
*/		Files::xfprintf_stderr("Places assigned\n");
	}
	l = 0;
	for (j = 0; j < P.NumCells; j++)
		if (l < Cells[j].n) l = Cells[j].n;
	SamplingQueue = (int**)Memory::xcalloc(P.NumThreads, sizeof(int*));
	P.InfQueuePeakLength = P.PopSize / P.NumThreads / INF_QUEUE_SCALE;
#pragma omp parallel for schedule(static,1) default(none) \
		shared(P, SamplingQueue, StateT, l)
	for (int i = 0; i < P.NumThreads; i++)
	{
		SamplingQueue[i] = (int*)Memory::xcalloc(2 * (MAX_PLACE_SIZE + CACHE_LINE_SIZE), sizeof(int));
		for (int k = 0; k < P.NumThreads; k++)
			StateT[i].inf_queue[k] = (Infection*)Memory::xcalloc(P.InfQueuePeakLength, sizeof(Infection));
		StateT[i].cell_inf = (float*)Memory::xcalloc(INT64_C(1) + l, sizeof(float));
		StateT[i].host_closure_queue = (HostClosure*)Memory::xcalloc(P.InfQueuePeakLength, sizeof(HostClosure));
	}

	//set up queues and storage for digital contact tracing
	if ((P.DoAdUnits) && (P.DoDigitalContactTracing))
	{
		for (int i = 0; i < P.NumAdunits; i++)
		{
			//malloc or calloc for these?
			AdUnits[i].dct = (int*)Memory::xcalloc(AdUnits[i].n, sizeof(int));
		}
		for (int i = 0; i < P.NumThreads; i++)
		{
			for (j = 0; j < P.NumAdunits; j++)
			{
				StateT[i].dct_queue[j] = (ContactEvent*)Memory::xcalloc(AdUnits[j].n, sizeof(ContactEvent));
			}
		}
	}

	//If outputting origin-destination matrix, set up storage for flow between admin units
	if ((P.DoAdUnits) && (P.DoOriginDestinationMatrix))
	{
		for (int i = 0; i < P.NumAdunits; i++)
		{
			AdUnits[i].origin_dest = (double*)Memory::xcalloc(MAX_ADUNITS, sizeof(double));
			for (j = 0; j < P.NumThreads; j++)
			{
				StateT[j].origin_dest[i] = (double*)Memory::xcalloc(MAX_ADUNITS, sizeof(double));
			}
			//initialise to zero
			for (j = 0; j < P.NumAdunits; j++)
			{
				AdUnits[i].origin_dest[j] = 0.0;
			}
		}
	}

	for (int i = 0; i < P.NumCells; i++)
	{
		Cells[i].cumTC = 0;
		Cells[i].S = Cells[i].n;
		Cells[i].L = Cells[i].I = 0;
	}
	Files::xfprintf_stderr("Allocated cell and host memory\n");
	Files::xfprintf_stderr("Assigned hosts to cells\n");

}

void SetupAirports(void)
{
	int k, l, m;
	double x, y, t, tmin;
	IndexList* base, *cur;

	Files::xfprintf_stderr("Assigning airports to microcells\n");
	// Convince static analysers that values are set correctly:
	if (!(P.DoAirports && P.HotelPlaceType < P.NumPlaceTypes)) ERR_CRITICAL("DoAirports || HotelPlaceType not set\n");

	P.Kernel = P.AirportKernel;
	P.KernelLookup.init(1.0, P.Kernel);
	CovidSim::TBD1::KernelLookup::init(P.KernelLookup, CellLookup, P.NumPopulatedCells);
	Airports[0].DestMcells = (IndexList*)Memory::xcalloc(_I64(P.NumPopulatedMicrocells) * NNA, sizeof(IndexList));
	base = (IndexList*)Memory::xcalloc(_I64(P.NumPopulatedMicrocells) * NNA, sizeof(IndexList));
	for (int i = 0; i < P.Nairports; i++) Airports[i].num_mcell = 0;
	cur = base;
	for (int i = 0; i < P.NumMicrocells; i++)
		if (Mcells[i].n > 0)
		{
			Mcells[i].AirportList = cur;
			cur += NNA;
		}

	FILE* stderr_shared = stderr;
#pragma omp parallel for private(k,l,x,y,t,tmin) schedule(static,10000) default(none) \
		shared(P, Airports, Mcells, stderr_shared)
	for (int i = 0; i < P.NumMicrocells; i++)
		if (Mcells[i].n > 0)
		{
			if (i % 10000 == 0) Files::xfprintf(stderr_shared, "\n%i           ", i);
			x = (((double)(i / P.total_microcells_high_)) + 0.5) * P.in_microcells_.width;
			y = (((double)(i % P.total_microcells_high_)) + 0.5) * P.in_microcells_.height;
			k = l = 0;
			tmin = 1e20;
			for (int j = 0; j < P.Nairports; j++)
				if (Airports[j].total_traffic > 0)
				{
					t = P.KernelLookup.num(dist2_raw(x, y, Airports[j].loc.x, Airports[j].loc.y)) * Airports[j].total_traffic;
					if (k < NNA)
					{
						Mcells[i].AirportList[k].id = j;
						Mcells[i].AirportList[k].prob = (float)t;
						if (t < tmin) { tmin = t; l = k; }
						k++;
					}
					else if (t > tmin)
					{
						Mcells[i].AirportList[l].id = j;
						Mcells[i].AirportList[l].prob = (float)t;
						tmin = 1e20;
						for (k = 0; k < NNA; k++)
							if (Mcells[i].AirportList[k].prob < tmin)
							{
								tmin = Mcells[i].AirportList[k].prob;
								l = k;
							}
					}
				}
			for (int j = 0; j < NNA; j++)
				Airports[Mcells[i].AirportList[j].id].num_mcell++;
		}
	cur = Airports[0].DestMcells;
	Files::xfprintf_stderr("Microcell airport lists collated.\n");
	for (int i = 0; i < P.Nairports; i++)
	{
		Airports[i].DestMcells = cur;
		cur += Airports[i].num_mcell;
		Airports[i].num_mcell = 0;
	}
#pragma omp parallel for private(k,l,t,tmin) schedule(static,10000) default(none) \
		shared(P, Airports, Mcells, stderr_shared)
	for (int i = 0; i < P.NumMicrocells; i++)
		if (Mcells[i].n > 0)
		{
			if (i % 10000 == 0) Files::xfprintf(stderr_shared, "\n%i           ", i);
			t = 0;
			for (int j = 0; j < NNA; j++)
			{
				t += Mcells[i].AirportList[j].prob;
				k = Mcells[i].AirportList[j].id;
#pragma omp critical (airport)
				l = (Airports[k].num_mcell++);
				Airports[k].DestMcells[l].id = i;
				Airports[k].DestMcells[l].prob = Mcells[i].AirportList[j].prob * ((float)Mcells[i].n);
			}
			tmin = 0;
			for (int j = 0; j < NNA; j++)
			{
				Mcells[i].AirportList[j].prob = (float)(tmin + Mcells[i].AirportList[j].prob / t);
				tmin = Mcells[i].AirportList[j].prob;
			}
		}
	Files::xfprintf_stderr("Airport microcell lists collated.\n");
	for (int i = 0; i < P.Nairports; i++)
		if (Airports[i].total_traffic > 0)
		{
			for (int j = 1; j < Airports[i].num_mcell; j++)
				Airports[i].DestMcells[j].prob += Airports[i].DestMcells[j - 1].prob;
			t = Airports[i].DestMcells[Airports[i].num_mcell - 1].prob;
			if (t == 0) t = 1.0;
			for (int j = 0; j < Airports[i].num_mcell - 1; j++)
				Airports[i].DestMcells[j].prob = (float)(Airports[i].DestMcells[j].prob / t);
			if (Airports[i].num_mcell > 0) Airports[i].DestMcells[Airports[i].num_mcell - 1].prob = 1.0;
			for (int j = l = 0; l <= 1024; l++)
			{
				t = ((double)l) / 1024.0;
				while (Airports[i].DestMcells[j].prob < t) j++;
				Airports[i].Inv_DestMcells[l] = j;
			}
			l = 0;
			for (int j = 0; j < Airports[i].num_mcell; j++)
				l += Mcells[Airports[i].DestMcells[j].id].NumPlacesByType[P.HotelPlaceType];
			if (l < 10)
			{
				Files::xfprintf_stderr("(%i ", l);
				l = 0;
				for (int j = 0; j < Airports[i].num_mcell; j++)
					l += Mcells[Airports[i].DestMcells[j].id].n;
				Files::xfprintf_stderr("%i %i) ", Airports[i].num_mcell, l);
			}
		}
	Files::xfprintf_stderr("\nInitialising hotel to airport lookup tables\n");
	Memory::xfree(base);
#pragma omp parallel for private(l,m,t,tmin) schedule(static,1) default(none) shared(P, Airports, Places, stderr_shared)
	for (int i = 0; i < P.Nairports; i++)
		if (Airports[i].total_traffic > 0)
		{
			m = (int)(Airports[i].total_traffic / HOTELS_PER_1000PASSENGER / 1000);
			if (m < MIN_HOTELS_PER_AIRPORT) m = MIN_HOTELS_PER_AIRPORT;
			Files::xfprintf(stderr_shared, "\n%i    ", i);
			tmin = MAX_DIST_AIRPORT_TO_HOTEL * MAX_DIST_AIRPORT_TO_HOTEL * 0.75;
			do
			{
				tmin += 0.25 * MAX_DIST_AIRPORT_TO_HOTEL * MAX_DIST_AIRPORT_TO_HOTEL;
				Airports[i].num_place = 0;
				for (int j = 0; j < P.Nplace[P.HotelPlaceType]; j++)
					if (dist2_raw(Airports[i].loc.x, Airports[i].loc.y,
						Places[P.HotelPlaceType][j].loc.x, Places[P.HotelPlaceType][j].loc.y) < tmin)
						Airports[i].num_place++;
			} while (Airports[i].num_place < m);
			if (tmin > MAX_DIST_AIRPORT_TO_HOTEL * MAX_DIST_AIRPORT_TO_HOTEL) Files::xfprintf(stderr_shared, "*** %i : %lg %i ***\n", i, sqrt(tmin), Airports[i].num_place);
			Airports[i].DestPlaces = (IndexList*)Memory::xcalloc(Airports[i].num_place, sizeof(IndexList));
			Airports[i].num_place = 0;
			for (int j = 0; j < P.Nplace[P.HotelPlaceType]; j++)
				if ((t = dist2_raw(Airports[i].loc.x, Airports[i].loc.y,
					Places[P.HotelPlaceType][j].loc.x, Places[P.HotelPlaceType][j].loc.y)) < tmin)
				{
					Airports[i].DestPlaces[Airports[i].num_place].prob = (float)P.KernelLookup.num(t);
					Airports[i].DestPlaces[Airports[i].num_place].id = j;
					Airports[i].num_place++;
				}
			t = 0;
			for (int j = 0; j < Airports[i].num_place; j++)
			{
				Airports[i].DestPlaces[j].prob = (float)(t + Airports[i].DestPlaces[j].prob);
				t = Airports[i].DestPlaces[j].prob;
			}
			for (int j = 0; j < Airports[i].num_place - 1; j++)
				Airports[i].DestPlaces[j].prob = (float)(Airports[i].DestPlaces[j].prob / t);
			if (Airports[i].num_place > 0) Airports[i].DestPlaces[Airports[i].num_place - 1].prob = 1.0;
			for (int j = l = 0; l <= 1024; l++)
			{
				t = ((double)l) / 1024.0;
				while (Airports[i].DestPlaces[j].prob < t) j++;
				Airports[i].Inv_DestPlaces[l] = j;
			}
		}
	for (int i = 0; i < P.Nplace[P.HotelPlaceType]; i++) Places[P.HotelPlaceType][i].n = 0;
	P.Kernel = P.MoveKernel;
	P.KernelLookup.init(1.0, P.Kernel);
	CovidSim::TBD1::KernelLookup::init(P.KernelLookup, CellLookup, P.NumPopulatedCells);
	Files::xfprintf_stderr("\nAirport initialisation completed successfully\n");
}

const double PROP_OTHER_PARENT_AWAY = 0.0;

void AssignHouseholdAges(int n, int pers, int tn, bool do_adunit_demog)
{
	/* Complex household age distribution model
		- picks number of children (nc)
		- tries to space them reasonably
		- picks parental ages to be consistent with childrens' and each other
		- other adults in large households are assumed to be grandparents
		- for Thailand, 2 person households are 95% couples without children, 5% 1 parent families
	*/
	int i, j, k, nc, ad;
	int a[MAX_HOUSEHOLD_SIZE + 2];

	ad = (do_adunit_demog && (P.DoAdUnits)) ? Mcells[Hosts[pers].mcell].adunit : 0;
	if (!P.DoHouseholds)
	{
		for (i = 0; i < n; i++)
			a[i] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
	}
	else
	{
		if (n == 1)
		{
			if (ranf_mt(tn) < P.OnePersHouseProbOld)
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[0] < P.NoChildPersAge)
					|| (ranf_mt(tn) > (((double)a[0]) - P.NoChildPersAge + 1) / (_I64(P.OldPersAge) - P.NoChildPersAge + 1)));
			}
			else if ((P.OnePersHouseProbYoung > 0) && (ranf_mt(tn) < P.OnePersHouseProbYoung / (1 - P.OnePersHouseProbOld)))
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				} while ((a[0] > P.YoungAndSingle) || (a[0] < P.MinAdultAge)
					|| (ranf_mt(tn) > 1 - P.YoungAndSingleSlope * (((double)a[0]) - P.MinAdultAge) / (_I64(P.YoungAndSingle) - P.MinAdultAge)));
			}
			else
				while ((a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))]) < P.MinAdultAge);
		}
		else if (n == 2)
		{
			if (ranf_mt(tn) < P.TwoPersHouseProbOld)
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[0] < P.NoChildPersAge)
					|| (ranf_mt(tn) > (((double)a[0]) - P.NoChildPersAge + 1) / (_I64(P.OldPersAge) - P.NoChildPersAge + 1)));
				do
				{
					a[1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[1] > a[0] + P.MaxMFPartnerAgeGap) || (a[1] < a[0] - P.MaxFMPartnerAgeGap) || (a[1] < P.NoChildPersAge)
					|| (ranf_mt(tn) > (((double)a[1]) - P.NoChildPersAge + 1) / (_I64(P.OldPersAge) - P.NoChildPersAge + 1)));
			}
			else if (ranf_mt(tn) < P.OneChildTwoPersProb / (1 - P.TwoPersHouseProbOld))
			{
				while ((a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))]) > P.MaxChildAge);
				do
				{
					a[1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[1] > a[0] + P.MaxParentAgeGap) || (a[1] < a[0] + P.MinParentAgeGap) || (a[1] < P.MinAdultAge));
			}
			else if ((P.TwoPersHouseProbYoung > 0) && (ranf_mt(tn) < P.TwoPersHouseProbYoung / (1 - P.TwoPersHouseProbOld - P.OneChildTwoPersProb)))
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				} while ((a[0] < P.MinAdultAge) || (a[0] > P.YoungAndSingle)
					|| (ranf_mt(tn) > 1 - P.YoungAndSingleSlope * (((double)a[0]) - P.MinAdultAge) / (_I64(P.YoungAndSingle) - P.MinAdultAge)));
				do
				{
					a[1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[1] > a[0] + P.MaxMFPartnerAgeGap) || (a[1] < a[0] - P.MaxFMPartnerAgeGap) || (a[1] < P.MinAdultAge));
			}
			else
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				} while (a[0] < P.MinAdultAge);
				do
				{
					a[1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[1] > a[0] + P.MaxMFPartnerAgeGap) || (a[1] < a[0] - P.MaxFMPartnerAgeGap) || (a[1] < P.MinAdultAge));
			}

		}
		else
		{
			if (n == 3)
			{
				if ((P.ZeroChildThreePersProb > 0) || (P.TwoChildThreePersProb > 0))
					nc = (ranf_mt(tn) < P.ZeroChildThreePersProb) ? 0 : ((ranf_mt(tn) < P.TwoChildThreePersProb) ? 2 : 1);
				else
					nc = 1;
			}
			else if (n == 4)
				nc = (ranf_mt(tn) < P.OneChildFourPersProb) ? 1 : 2;
			else if (n == 5)
				nc = (ranf_mt(tn) < P.ThreeChildFivePersProb) ? 3 : 2;
			else
				nc = n - 2 - (int)(3 * ranf_mt(tn));
			if (nc <= 0)
			{
				do
				{
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
					a[1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[1] < P.MinAdultAge) || (a[0] < P.MinAdultAge));
				do
				{
					a[2] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
				}
				while ((a[2] >= a[1] + P.MaxMFPartnerAgeGap) || (a[2] < a[1] - P.MaxFMPartnerAgeGap));
			}
			else
			{
				do
				{
					a[0] = 0;
					for (i = 1; i < nc; i++)
						a[i] = a[i - 1] + 1 + ((int)ignpoi_mt((double) (_I64(P.MeanChildAgeGap) - 1), tn));
					a[0] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))] - a[(int)(ranf_mt(tn) * ((double)nc))];
					for (i = 1; i < nc; i++) a[i] += a[0];
					k = (((nc == 1) && (ranf_mt(tn) < P.OneChildProbYoungestChildUnderFive)) || ((nc == 2) && (ranf_mt(tn) < P.TwoChildrenProbYoungestUnderFive))
						|| ((nc > 2) && (ranf_mt(tn) < P.ProbYoungestChildUnderFive))) ? 5 : P.MaxChildAge;
				} while ((a[0] < 0) || (a[0] > k) || (a[nc - 1] > P.MaxChildAge));
				j = a[nc - 1] - a[0] - (P.MaxParentAgeGap - P.MinParentAgeGap);
				if (j > 0)
					j += P.MaxParentAgeGap;
				else
					j = P.MaxParentAgeGap;
				do
				{
					a[nc] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
					k = a[nc - 1];
				} while ((a[nc] > a[0] + j) || (a[nc] < k + P.MinParentAgeGap) || (a[nc] < P.MinAdultAge));
				if ((n > nc + 1) && (ranf_mt(tn) > PROP_OTHER_PARENT_AWAY))
				{
					do
					{
						a[nc + 1] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))];
					} while ((a[nc + 1] > a[nc] + P.MaxMFPartnerAgeGap) || (a[nc + 1] < a[nc] - P.MaxFMPartnerAgeGap)
						|| (a[nc + 1] > a[0] + j) || (a[nc + 1] < k + P.MinParentAgeGap) || (a[nc + 1] < P.MinAdultAge));
				}

				if (n > nc + 2)
				{
					j = ((a[nc + 1] > a[nc]) ? a[nc + 1] : a[nc]) + P.OlderGenGap;
					if (j >= NUM_AGE_GROUPS * AGE_GROUP_WIDTH) j = NUM_AGE_GROUPS * AGE_GROUP_WIDTH - 1;
					if (j < P.NoChildPersAge) j = P.NoChildPersAge;
					for (i = nc + 2; i < n; i++)
						while ((a[i] = State.InvAgeDist[ad][(int)(1000.0 * ranf_mt(tn))]) < j);
				}
			}
		}
	}
	for (i = 0; i < n; i++) Hosts[pers + i].age = (unsigned char) a[i];
}

void AssignPeopleToPlaces()
{
	int i2, j, j2, k, k2, l, m, tp, f, f2, f3, f4, ic, a, cnt, ca, nn;
	int* PeopleArray;
	int* NearestPlaces[MAX_NUM_THREADS];
	double s, t, *NearestPlacesProb[MAX_NUM_THREADS];
	Cell* ct;
	int npt;

	npt = MAX_NUM_PLACE_TYPES;

	if (P.DoPlaces)
	{
		Files::xfprintf_stderr("Assigning people to places....\n");
		for (int i = 0; i < P.NumCells; i++)
		{
			Cells[i].infected = Cells[i].susceptible;
			Cells[i].susceptible = (int*)Memory::xcalloc(Cells[i].n, sizeof(int));
			Cells[i].cumTC = Cells[i].n;
		}

		//PropPlaces initialisation is only valid for non-overlapping places.
		for (int i = 0; i < P.PopSize; i++)
		{
			for (tp = 0; tp < npt; tp++) //Changed from 'for(tp=0;tp<P.NumPlaceTypes;tp++)' to try and assign -1 early and avoid problems when using less than the default number of placetypes later
			{
				Hosts[i].PlaceLinks[tp] = -1;
			}
		}

		for (tp = 0; tp < P.NumPlaceTypes; tp++)
		{
			if (tp != P.HotelPlaceType)
			{
				cnt = 0;
				for (a = 0; a < P.NumPopulatedCells; a++)
				{
					Cell *c = CellLookup[a];
					c->n = 0;
					for (j = 0; j < c->cumTC; j++)
					{
						k = HOST_AGE_YEAR(c->members[j]);
						f = ((PropPlaces[k][tp] > 0) && (ranf() < PropPlaces[k][tp]));
						if (f)
							for (k = 0; (k < tp) && (f); k++)
								if (Hosts[c->members[j]].PlaceLinks[k] >= 0) f = 0; //(ranf()<P.PlaceExclusivityMatrix[tp][k]);
						// Am assuming people can only belong to 1 place (and a hotel) at present
						if (f)
						{
							c->susceptible[c->n] = c->members[j];
							(c->n)++;
							cnt++;
						}
					}
					c->S = c->n;
					c->I = 0;
				}
				PeopleArray = (int*)Memory::xcalloc(cnt, sizeof(int));
				j2 = 0;
				for (a = 0; a < P.NumPopulatedCells; a++)
				{
					Cell *c = CellLookup[a];
					for (j = 0; j < c->n; j++)
					{
						PeopleArray[j2] = c->susceptible[j];
						j2++;
					}
				}
				// Use the Fisher–Yates shuffle algorithm to get a random permutation of PeopleArray
				for (int index1 = cnt - 1; index1 > 0; index1--)
				{
					int index2 = (int)(((double)(index1 + 1)) * ranf());
					int tmp = PeopleArray[index1];
					PeopleArray[index1] = PeopleArray[index2];
					PeopleArray[index2] = tmp;
				}
				m = 0;
				if (tp < P.nsp)
				{
					for (int i = 0; i < P.Nplace[tp]; i++)
					{
						m += (int)(Places[tp][i].treat_end_time = (unsigned short)Places[tp][i].n);
						Places[tp][i].n = 0;
					}
				}
				else if (P.PlaceTypeSizePower[tp] == 0 && P.PlaceTypeSizeSD[tp] == 0)
				{
					for (int i = 0; i < P.Nplace[tp]; i++)
					{
						Places[tp][i].n = 0;
						j = 1 + ((int)ignpoi(P.PlaceTypeMeanSize[tp] - 1));
						if (j > USHRT_MAX - 1) j = USHRT_MAX - 1;
						m += (int)(Places[tp][i].treat_end_time = (unsigned short)j);
					}
				}
				//added this code to allow a place size to be specified according to a lognormal distribution - ggilani 09/02/17
				else if (P.PlaceTypeSizePower[tp] == 0 && P.PlaceTypeSizeSD[tp] > 0)
				{
					for (int i = 0; i < P.Nplace[tp]; i++)
					{
						Places[tp][i].n = 0;
						do
						{
							j = (int)gen_lognormal(P.PlaceTypeMeanSize[tp], P.PlaceTypeSizeSD[tp]);
						}
						while (j<P.PlaceTypeSizeMin[tp] || j>P.PlaceTypeSizeMax[tp]);
						if (j > USHRT_MAX - 1) j = USHRT_MAX - 1;
						m += (int)(Places[tp][i].treat_end_time = (unsigned short)j);
					}
				}
				else
				{
					s = pow(P.PlaceTypeSizeOffset[tp] / (P.PlaceTypeSizeOffset[tp] + P.PlaceTypeSizeMax[tp] - P.PlaceTypeSizeMin[tp]), P.PlaceTypeSizePower[tp]);
					for (int i = 0; i < P.Nplace[tp]; i++)
					{
						j = (int)floor(P.PlaceTypeSizeOffset[tp] * pow((1 - s) * ranf() + s, -1 / P.PlaceTypeSizePower[tp]) + P.PlaceTypeSizeMin[tp] - P.PlaceTypeSizeOffset[tp]);
						if (j > USHRT_MAX - 1) j = USHRT_MAX - 1;
						m += (int)(Places[tp][i].treat_end_time = (unsigned short)j);
						Places[tp][i].n = 0;
					}
				}
				if (tp < P.nsp)
				{
					t = ((double)m) / ((double)P.Nplace[tp]);
					Files::xfprintf_stderr("Adjusting place weights by cell (Capacity=%i Demand=%i  Av place size=%lg)\n", m, cnt, t);
					for (int i = 0; i < P.Nplace[tp]; i++)
						if (Places[tp][i].treat_end_time > 0)
						{
							j = (int)(Places[tp][i].loc.x / P.in_cells_.width);
							k = j * P.nch + ((int)(Places[tp][i].loc.y / P.in_cells_.height));
							Cells[k].I += (int)Places[tp][i].treat_end_time;
						}
					for (k = 0; k < P.NumCells; k++)
					{
						int i = k % P.nch;
						j = k / P.nch;
						f2 = Cells[k].I; f3 = Cells[k].S;
						if ((i > 0) && (j > 0))
						{
							f2 += Cells[(j - 1) * P.nch + (i - 1)].I; f3 += Cells[(j - 1) * P.nch + (i - 1)].S;
						}
						if (i > 0)
						{
							f2 += Cells[j * P.nch + (i - 1)].I; f3 += Cells[j * P.nch + (i - 1)].S;
						}
						if ((i > 0) && (j < P.ncw - 1))
						{
							f2 += Cells[(j + 1) * P.nch + (i - 1)].I; f3 += Cells[(j + 1) * P.nch + (i - 1)].S;
						}
						if (j > 0)
						{
							f2 += Cells[(j - 1) * P.nch + i].I; f3 += Cells[(j - 1) * P.nch + i].S;
						}
						if (j < P.ncw - 1)
						{
							f2 += Cells[(j + 1) * P.nch + i].I; f3 += Cells[(j + 1) * P.nch + i].S;
						}
						if ((i < P.nch - 1) && (j > 0))
						{
							f2 += Cells[(j - 1) * P.nch + (i + 1)].I; f3 += Cells[(j - 1) * P.nch + (i + 1)].S;
						}
						if (i < P.nch - 1)
						{
							f2 += Cells[j * P.nch + (i + 1)].I; f3 += Cells[j * P.nch + (i + 1)].S;
						}
						if ((i < P.nch - 1) && (j < P.ncw - 1))
						{
							f2 += Cells[(j + 1) * P.nch + (i + 1)].I; f3 += Cells[(j + 1) * P.nch + (i + 1)].S;
						}
						Cells[k].L = f3; Cells[k].R = f2;
					}
					m = f2 = f3 = f4 = 0;
					for (k = 0; k < P.NumCells; k++)
						if ((Cells[k].S > 0) && (Cells[k].I == 0))
						{
							f2 += Cells[k].S; f3++;
							if (Cells[k].R == 0) f4 += Cells[k].S;
						}
					Files::xfprintf_stderr("Demand in cells with no places=%i in %i cells\nDemand in cells with no places <=1 cell away=%i\n", f2, f3, f4);
					for (int i = 0; i < P.Nplace[tp]; i++)
						if (Places[tp][i].treat_end_time > 0)
						{
							j = (int)(Places[tp][i].loc.x / P.in_cells_.width);
							k = j * P.nch + ((int)(Places[tp][i].loc.y / P.in_cells_.height));
							if ((Cells[k].L > 0) && (Cells[k].R > 0))
							{
								s = ((double)Cells[k].L) / ((double)Cells[k].R);
								Places[tp][i].treat_end_time = (unsigned short)ceil(Places[tp][i].treat_end_time * s);
							}
							m += ((int)Places[tp][i].treat_end_time);
						}
					for (int i = 0; i < P.NumCells; i++) Cells[i].L = Cells[i].I = Cells[i].R = 0;
				}
				t = ((double)m) / ((double)P.Nplace[tp]);
				Files::xfprintf_stderr("Adjusting place weights (Capacity=%i Demand=%i  Av place size=%lg)\n", m, cnt, t);
				for (int i = m = 0; i < P.Nplace[tp]; i++)
				{
					s = ((double)Places[tp][i].treat_end_time) * 43 / 40 - 1;
					m += (int)(Places[tp][i].treat_end_time = (unsigned short)(1.0 + ignpoi(s)));
				}
				if (tp < P.nsp)
					s = ((double)cnt) * 1.075;
				else
					s = ((double)cnt) * 1.125;
				j2 = ((int)s) - m;
				for (int i = 0; i < j2; i++)
				{
					Places[tp][(int)(((double)P.Nplace[tp]) * ranf())].treat_end_time++;
				}
				j2 = -j2;
				for (int i = 0; i < j2; i++)
				{
					while (Places[tp][j = (int)(((double)P.Nplace[tp]) * ranf())].treat_end_time < 2);
					Places[tp][j].treat_end_time--;
				}
				if (P.PlaceTypeNearestNeighb[tp] == 0)
				{
					for (int i = 0; i < P.NumCells; i++) Cells[i].S = 0;
					for (j = 0; j < P.Nplace[tp]; j++)
					{
						int i = P.nch * ((int)(Places[tp][j].loc.x / P.in_cells_.width)) + ((int)(Places[tp][j].loc.y / P.in_cells_.height));
						Cells[i].S += (int)Places[tp][j].treat_end_time;
					}
					for (int i = 0; i < P.NumCells; i++)
					{
						if (Cells[i].S > Cells[i].cumTC)
						{
							Memory::xfree(Cells[i].susceptible);
							Cells[i].susceptible = (int*)Memory::xcalloc(Cells[i].S, sizeof(int));
						}
						Cells[i].S = 0;
					}
					for (j = 0; j < P.Nplace[tp]; j++)
					{
						int i = P.nch * ((int)(Places[tp][j].loc.x / P.in_cells_.width)) + ((int)(Places[tp][j].loc.y / P.in_cells_.height));
						k = (int)Places[tp][j].treat_end_time;
						for (j2 = 0; j2 < k; j2++)
						{
							Cells[i].susceptible[Cells[i].S] = j;
							Cells[i].S++;
						}
					}
				}
				for (int i = 0; i < P.NumThreads; i++)
				{
					NearestPlaces[i] = (int*)Memory::xcalloc(_I64(P.PlaceTypeNearestNeighb[tp]) + CACHE_LINE_SIZE, sizeof(int));
					NearestPlacesProb[i] = (double*)Memory::xcalloc(_I64(P.PlaceTypeNearestNeighb[tp]) + CACHE_LINE_SIZE, sizeof(double));
				}
				P.Kernel.type_ = P.PlaceTypeKernelType[tp];
				P.Kernel.scale_ = P.PlaceTypeKernelScale[tp];
				P.Kernel.shape_ = P.PlaceTypeKernelShape[tp];
				P.Kernel.p3_ = P.PlaceTypeKernelP3[tp];
				P.Kernel.p4_ = P.PlaceTypeKernelP4[tp];
				P.KernelLookup.init(1.0, P.Kernel);
				CovidSim::TBD1::KernelLookup::init(P.KernelLookup, CellLookup, P.NumPopulatedCells);
				UpdateProbs(1);
				ca = 0;
				Files::xfprintf_stderr("Allocating people to place type %i\n", tp);
				a = cnt;
				nn = P.PlaceTypeNearestNeighb[tp];
				if (P.PlaceTypeNearestNeighb[tp] > 0)
				{
					int tn = 0;
					for (j = 0; j < a; j++)
					{
						if (j % 1000 == 0) Files::xfprintf_stderr("(%i) %i      \r", tp, j);
						for (i2 = 0; i2 < nn; i2++)	NearestPlacesProb[tn][i2] = 0;
						l = 1; k = m = f2 = 0;
						int i = PeopleArray[j];
						ic = Hosts[i].mcell;

						MicroCellPosition mc_position = P.get_micro_cell_position_from_cell_index(ic);
						Direction m2 = Direction::Right;
						if (Hosts[i].PlaceLinks[tp] < 0) //added this so that if any hosts have already be assigned due to their household membership, they will not be reassigned
						{
							auto const host_country = mcell_country[Hosts[i].mcell];
							while (((k < nn) || (l < 4)) && (l < P.total_microcells_wide_))
							{
								if (P.is_in_bounds(mc_position))
								{
									ic = P.get_micro_cell_index_from_position(mc_position);
									if (mcell_country[ic] == host_country)
									{
										auto const& cur_cell = Mcells[ic];
										auto const place_type_count = cur_cell.NumPlacesByType[tp];
										for (cnt = 0; cnt < place_type_count; cnt++)
										{
											auto const place_idx = cur_cell.places[tp][cnt];
											if (place_idx >= P.Nplace[tp]) Files::xfprintf_stderr("#%i %i %i  ", tp, ic, cnt);
											auto const& cur_place = Places[tp][place_idx];
											t = dist2_raw(Households[Hosts[i].hh].loc.x, Households[Hosts[i].hh].loc.y,
												cur_place.loc.x, cur_place.loc.y);
											s = P.KernelLookup.num(t);
											if (tp < P.nsp)
											{
												t = ((double)cur_place.treat_end_time);
												if (HOST_AGE_YEAR(i) < P.PlaceTypeMaxAgeRead[tp])
												{
													if ((t > 0) && (cur_place.AvailByAge[HOST_AGE_YEAR(i)] > 0))
														s *= t;
													else
														s = 0;
												}
												else if (t > 0)
													s *= t;
											}
											k2 = 0;
											j2 = 0;
											t = 1e10;
											if (s > 0)
											{
												if (k < nn)
												{
													NearestPlaces[tn][k] = place_idx;
													NearestPlacesProb[tn][k] = s;
													k++;
												}
												else
												{
													for (i2 = 0; i2 < nn; i2++)
													{
														if (NearestPlacesProb[tn][i2] < t)
														{
															t = NearestPlacesProb[tn][i2]; j2 = i2;
														}
													}
													if (s > t)
													{
														NearestPlacesProb[tn][j2] = s;
														NearestPlaces[tn][j2] = place_idx;
													}
												}
											}
										}
									}
								}
								mc_position += m2;
								f2 = (f2 + 1) % l;
								if (f2 == 0)
								{
									m2 = rotate_left(m2);
									m = (m + 1) % 2;
									if (m == 0) l++;
								}
							}

							s = 0;
							if (k > nn) Files::xfprintf_stderr("*** k>P.PlaceTypeNearestNeighb[tp] ***\n");
							if (k == 0)
							{
								Files::xfprintf_stderr("# %i %i     \r", i, j);
								Hosts[i].PlaceLinks[tp] = -1;
							}
							else
							{
								for (i2 = 1; i2 < k; i2++)
									NearestPlacesProb[tn][i2] += NearestPlacesProb[tn][i2 - 1];
								s = NearestPlacesProb[tn][k - 1];
								t = ranf_mt(tn);
								f = 0;
								for (i2 = 0; (i2 < k) && (!f); i2++)
								{
									if ((f = (t < NearestPlacesProb[tn][i2] / s)))
									{
										Hosts[i].PlaceLinks[tp] = NearestPlaces[tn][i2];
										ca++;
										if (tp < P.nsp)
											Places[tp][Hosts[i].PlaceLinks[tp]].treat_end_time--;
									}
									if (!f) Hosts[i].PlaceLinks[tp] = -1;
									if (NearestPlaces[tn][i2] >= P.Nplace[tp]) Files::xfprintf_stderr("@%i %i %i  ", tp, i, j);
								}
							}
						}
					}
				}
				else
				{
					k2 = cnt - ca;
					int m2 = cnt;
					a = k2 / 1000;
					f = k2;
					for (ic = 0; ic <= 30; ic++)
					{
						UpdateProbs(1);
						m2 = f - 1;
						if (ic < 9)
							f = 100 * (9 - ic) * a;
						else if (ic < 18)
							f = 10 * (18 - ic) * a;
						else if (ic < 27)
							f = (27 - ic) * a;
						else
						{
							m2 = k2 - 1;
							f = 0;
						}

						for (i2 = m2; i2 >= f; i2--)
						{
							int tn = 0;
							if (i2 % 10000 == 0)
								Files::xfprintf_stderr("(%i) %i            \r", tp, i2);
							k = PeopleArray[i2];
							int i = Hosts[k].pcell;
							f2 = 1;
							f3 = (HOST_AGE_YEAR(k) >= P.PlaceTypeMaxAgeRead[tp]);
							if (Hosts[k].PlaceLinks[tp] < 0)
								while ((f2 > 0) && (f2 < 1000))
								{
									do
									{
										s = ranf_mt(tn);
										l = Cells[i].InvCDF[(int)floor(s * 1024)];
										while (Cells[i].cum_trans[l] < s) l++;
										ct = CellLookup[l];
										m = (int)(ranf_mt(tn) * ((double)ct->S));
										j = -1;
										if (ct->susceptible[m] >= 0)
											if ((f3) || (Places[tp][ct->susceptible[m]].AvailByAge[HOST_AGE_YEAR(k)] > 0))
											{
												j = ct->susceptible[m];
												ct->susceptible[m] = -1;
											}
									} while (j < 0);
									if (j >= P.Nplace[tp])
									{
										Files::xfprintf_stderr("*%i %i: %i %i\n", k, tp, j, P.Nplace[tp]);
										ERR_CRITICAL("Out of bounds place link\n");
									}
									t = dist2_raw(Households[Hosts[k].hh].loc.x, Households[Hosts[k].hh].loc.y, Places[tp][j].loc.x, Places[tp][j].loc.y);
									s = ((double)ct->S) / ((double)ct->S0) * P.KernelLookup.num(t) / Cells[i].max_trans[l];
									if ((P.DoAdUnits) && (P.InhibitInterAdunitPlaceAssignment[tp] > 0))
									{
										if (Mcells[Hosts[k].mcell].adunit != Mcells[Places[tp][j].mcell].adunit) s *= (1 - P.InhibitInterAdunitPlaceAssignment[tp]);
									}
									if (ranf_mt(tn) < s)
									{
										l = (--ct->S);
										if (m < l) ct->susceptible[m] = ct->susceptible[l];
										Places[tp][j].treat_end_time--;
										ca++;
										Hosts[k].PlaceLinks[tp] = j;
										f2 = 0;
									}
									else
									{
										ct->susceptible[m] = j;
										f2++;
									}
								}
						}
					}
				}
				Files::xfprintf_stderr("%i hosts assigned to placetype %i\n", ca, tp);
				Memory::xfree(PeopleArray);
				for (int i = 0; i < P.Nplace[tp]; i++)
				{
					Places[tp][i].treat_end_time = 0;
					Places[tp][i].n = 0;
				}
				for (int i = 0; i < P.NumThreads; i++)
				{
					Memory::xfree(NearestPlacesProb[i]);
					Memory::xfree(NearestPlaces[i]);
				}
			}
		}
		for (int i = 0; i < P.NumCells; i++)
		{
			Cells[i].n = Cells[i].cumTC;
			Cells[i].cumTC = 0;
			Cells[i].S = Cells[i].I = Cells[i].L = Cells[i].R = 0;
			Memory::xfree(Cells[i].susceptible);
			Cells[i].susceptible = Cells[i].infected;
		}
	}
}

void StratifyPlaces(void)
{
	if (P.DoPlaces)
	{
		Files::xfprintf_stderr("Initialising groups in places\n");
#pragma omp parallel for schedule(static,500) default(none) \
			shared(P, Hosts)
		for (int i = 0; i < P.PopSize; i++)
			for (int j = 0; j < MAX_NUM_PLACE_TYPES; j++)
				Hosts[i].PlaceGroupLinks[j] = 0;
		for (int j = 0; j < P.NumPlaceTypes; j++)
			for (int i = 0; i < P.Nplace[j]; i++)
				Places[j][i].n = 0;
#pragma omp parallel for schedule(static,1) default(none) \
			shared(P, Places, Hosts)
		for (int tn = 0; tn < P.NumThreads; tn++)
			for (int j = tn; j < P.NumPlaceTypes; j += P.NumThreads)
			{
				if (j == P.HotelPlaceType)
				{
					int l = 2 * ((int)P.PlaceTypeMeanSize[j]);
					for (int i = 0; i < P.Nplace[j]; i++)
					{
						Places[j][i].members = (int*)Memory::xcalloc(l, sizeof(int));
						Places[j][i].n = 0;
					}
				}
				else
				{
					for (int i = 0; i < P.PopSize; i++)
					{
						if (Hosts[i].PlaceLinks[j] >= 0)
							Places[j][Hosts[i].PlaceLinks[j]].n++;
					}
					for (int i = 0; i < P.Nplace[j]; i++)
					{
						if (Places[j][i].n > 0)
						{
							Places[j][i].members = (int*)Memory::xcalloc(Places[j][i].n, sizeof(int));
						}
						Places[j][i].n = 0;
					}
					for (int i = 0; i < P.PopSize; i++)
					{
						int k = Hosts[i].PlaceLinks[j];
						if (k >= 0)
						{
							Places[j][k].members[Places[j][k].n] = i;
							Places[j][k].n++;
						}
					}
					for (int i = 0; i < P.Nplace[j]; i++)
						if (Places[j][i].n > 0)
						{
							double t = ((double)Places[j][i].n) / P.PlaceTypeGroupSizeParam1[j] - 1.0;
							if (t < 0)
								Places[j][i].ng = 1;
							else
								Places[j][i].ng = 1 + (int)ignpoi_mt(t, tn);
							Places[j][i].group_start = (int*)Memory::xcalloc(Places[j][i].ng, sizeof(int));
							Places[j][i].group_size = (int*)Memory::xcalloc(Places[j][i].ng, sizeof(int));
							int m = Places[j][i].n - Places[j][i].ng;
							int l;
							for (int k = l = 0; k < Places[j][i].ng; k++)
							{
								t = 1 / ((double)(_I64(Places[j][i].ng) - k));
								Places[j][i].group_start[k] = l;
								Places[j][i].group_size[k] = 1 + ignbin_mt((int32_t)m, t, tn);
								m -= (Places[j][i].group_size[k] - 1);
								l += Places[j][i].group_size[k];
							}
							for (int k = 0; k < Places[j][i].n; k++)
							{
								l = (int)(((double)Places[j][i].n) * ranf_mt(tn));
								int n = Places[j][i].members[l];
								Places[j][i].members[l] = Places[j][i].members[k];
								Places[j][i].members[k] = n;
							}
							for (int k = l = 0; k < Places[j][i].ng; k++)
								for (m = 0; m < Places[j][i].group_size[k]; m++)
								{
									Hosts[Places[j][i].members[l]].PlaceGroupLinks[j] = k;
									l++;
								}
						}
				}
			}

#pragma omp parallel for schedule(static,1) default (none) \
			shared(P, Places, StateT)
		for (int i = 0; i < P.NumThreads; i++)
		{
			for (int k = 0; k < P.NumPlaceTypes; k++)
			{
				if (P.DoPlaceGroupTreat)
				{
					int l = 0;
					for (int j = 0; j < P.Nplace[k]; j++)
						l += (int)Places[k][j].ng;
					StateT[i].p_queue[k] = (int*)Memory::xcalloc(l, sizeof(int));
					StateT[i].pg_queue[k] = (int*)Memory::xcalloc(l, sizeof(int));
				}
				else
				{
					StateT[i].p_queue[k] = (int*)Memory::xcalloc(P.Nplace[k], sizeof(int));
					StateT[i].pg_queue[k] = (int*)Memory::xcalloc(P.Nplace[k], sizeof(int));
				}
			}
		}
		Files::xfprintf_stderr("Groups initialised\n");
		/*		s2=t2=0;
				for(j=0;j<P.NumPlaceTypes;j++)
					{
					t=s=0;
					for(i=0;i<P.Nplace[j];i++)
						if(Places[j][i].ng>0)
							{
							for(k=0;k<Places[j][i].ng;k++)
								t+=(double) Places[j][i].group_size[k];
							s+=(double) Places[j][i].ng;
							}
					s2+=s;
					t2+=t;
					Files::xfprintf_stderr("Mean group size for place type %i = %lg\n",j,t/s);
					}
				t=0;
				for(i=0;i<P.PopSize;i++)
					for(j=0;j<P.NumPlaceTypes;j++)
						if(Hosts[i].PlaceLinks[j]>=0)
							t+=(double) Places[j][Hosts[i].PlaceLinks[j]].group_size[Hosts[i].PlaceGroupLinks[j]];
				Files::xfprintf_stderr("Overall mean group size = %lg (%lg)\n",t/((double) P.PopSize),t2/s2);
		*/
	}
}

void LoadPeopleToPlaces(std::string const& load_network_file)
{
	int i, j, k, l, m, n, npt, i2;
	int32_t s1, s2;
	FILE* dat;
	int fileversion;

	dat = Files::xfopen(load_network_file.c_str(), "rb");
	Files::fread_big(&fileversion, sizeof(fileversion), 1, dat);
	if (fileversion != NETWORK_FILE_VERSION)
	{
		ERR_CRITICAL("Incompatible network file - please rebuild using '/S:'.\n");
	}

	npt = P.PlaceTypeNoAirNum;
	Files::fread_big(&i, sizeof(int), 1, dat);
	Files::fread_big(&j, sizeof(int), 1, dat);
	Files::fread_big(&s1, sizeof(int32_t), 1, dat);
	Files::fread_big(&s2, sizeof(int32_t), 1, dat);
	if (i != npt) ERR_CRITICAL("Number of place types does not match saved value\n");
	if (j != P.PopSize) ERR_CRITICAL("Population size does not match saved value\n");
	if ((s1 != P.setupSeed1) || (s2 != P.setupSeed2))
	{
		ERR_CRITICAL_FMT("Random number seeds do not match saved values: %" PRId32 " != %" PRId32 " || %" PRId32 " != %" PRId32 "\n", s1, P.setupSeed1, s2, P.setupSeed2);
	}
	k = (P.PopSize + 999999) / 1000000;
	for (i = 0; i < P.PopSize; i++)
		for (j = 0; j < P.NumPlaceTypes; j++)
			Hosts[i].PlaceLinks[j] = -1;
	for (i = i2 = 0; i < k; i++)
	{
		l = (i < k - 1) ? 1000000 : (P.PopSize - 1000000 * (k - 1));
		Files::fread_big(&netbuf, sizeof(int), _I64(npt) * l, dat);
		for (j = 0; j < l; j++)
		{
			n = j * npt;
			for (m = 0; m < npt; m++)
			{
				Hosts[i2].PlaceLinks[m] = netbuf[n + m];
				if (Hosts[i2].PlaceLinks[m] >= P.Nplace[m])
				{
					Files::xfprintf_stderr("*%i %i: %i %i\n", i2, m, Hosts[i2].PlaceLinks[m], P.Nplace[m]);
					ERR_CRITICAL("Out of bounds place link\n");
				}
			}
			i2++;
		}
		Files::xfprintf_stderr("%i loaded            \r", i * 1000000 + l);
	}

	/*	for(i=0;i<P.PopSize;i++)
			{
			if((i+1)%100000==0) Files::xfprintf_stderr("%i loaded            \r",i+1);
			fread_big(&(Hosts[i].PlaceLinks[0]),sizeof(int),P.NumPlaceTypes,dat);
			}
	*/	Files::xfprintf_stderr("\n");
	Files::xfclose(dat);
}
void SavePeopleToPlaces(std::string const& save_network_file)
{
	int i, j, npt;
	FILE* dat;
	int fileversion = NETWORK_FILE_VERSION;

	npt = P.PlaceTypeNoAirNum;
	dat = Files::xfopen(save_network_file.c_str(), "wb");
	Files::fwrite_big(&fileversion, sizeof(fileversion), 1, dat);

	if (P.NumPlaceTypes > 0)
	{
		Files::fwrite_big(&npt, sizeof(int), 1, dat);
		Files::fwrite_big(&(P.PopSize), sizeof(int), 1, dat);
		Files::fwrite_big(&P.setupSeed1, sizeof(int32_t), 1, dat);
		Files::fwrite_big(&P.setupSeed2, sizeof(int32_t), 1, dat);
		for (i = 0; i < P.PopSize; i++)
		{
			if ((i + 1) % 100000 == 0) Files::xfprintf_stderr("%i saved            \r", i + 1);
			/*			Files::fwrite_big(&(Hosts[i].spatial_norm),sizeof(float),1,dat);
			*/			Files::fwrite_big(&(Hosts[i].PlaceLinks[0]), sizeof(int), npt, dat);
			for (j = 0; j < npt; j++)
				if (Hosts[i].PlaceLinks[j] >= P.Nplace[j])
				{
					Files::xfprintf_stderr("*%i %i: %i %i\n", i, j, Hosts[i].PlaceLinks[j], P.Nplace[j]);
					ERR_CRITICAL("Out of bounds place link\n");
				}
		}
	}

	Files::xfprintf_stderr("\n");
	Files::xfclose(dat);
}

void SaveAgeDistrib(std::string const& output_file_base)
{
	int i;
	FILE* dat;
	std::string outname;

	outname = output_file_base + ".agedist.xls";
	dat = Files::xfopen(outname.c_str(), "wb");
	if (P.DoDeath)
	{
		Files::xfprintf(dat, "age\tfreq\tlifeexpect\n");
		for (i = 0; i < NUM_AGE_GROUPS; i++)
			Files::xfprintf(dat, "%i\ta%.10f\t%.10f\n", i, AgeDist[i], AgeDist2[i]);
		Files::xfprintf(dat, "\np\tlife_expec\tage\n");
		for (i = 0; i <= 1000; i++)
			Files::xfprintf(dat, "%.10f\t%.10f\t%i\n", ((double)i) / 1000, P.InvLifeExpecDist[0][i], State.InvAgeDist[0][i]);
	}
	else
	{
		Files::xfprintf(dat, "age\tfreq\n");
		for (i = 0; i < NUM_AGE_GROUPS; i++)
			Files::xfprintf(dat, "%i\t%.10f\n", i, AgeDist[i]);
	}

	Files::xfclose(dat);
}
