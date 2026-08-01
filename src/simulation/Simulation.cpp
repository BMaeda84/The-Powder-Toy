#include "Simulation.h"
#include "Air.h"
#include "ElementClasses.h"
#include "TransitionConstants.h"
#include "gravity/Gravity.h"
#include "ToolClasses.h"
#include "SimulationData.h"
#include "client/GameSave.h"
#include "common/tpt-rand.h"
#include "common/Defer.h"
#include "FrameTime.h"
#include "gui/game/Brush.h"
#include "elements/EMP.h"
#include "elements/LOLZ.h"
#include "elements/STKM.h"
#include "elements/PIPE.h"
#include "elements/FILT.h"
#include "elements/PRTI.h"
#include "elements/PLNT.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <set>
#include <stack>
#include <vector>

namespace
{
	using ParticleCostClock = std::chrono::steady_clock;

	enum class ParticleCostClass : size_t
	{
		LoopDead,
		Powder,
		SolidLocal,
		SolidSpecial,
		Liquid,
		Gas,
		Energy,
		ActorSpecial,
		StateOther,
		Count,
	};

	constexpr auto ParticleCostClassCount = size_t(ParticleCostClass::Count);
	constexpr std::array<const char *, ParticleCostClassCount> particleCostClassNames = {
		"loop_dead", "powder", "solid_local", "solid_special", "liquid",
		"gas", "energy", "actor_special", "state_other",
	};

	struct ParticleCostFrame
	{
		uint64_t updateCalls = 0;
		uint64_t updateNs = 0;
		uint64_t loopNs = 0;
		uint64_t slotsSeen = 0;
		uint64_t liveSeen = 0;
		uint64_t deadSlots = 0;
		uint64_t classSwitches = 0;
		uint64_t tryMoveCalls = 0;
		uint64_t doMoveCalls = 0;
		uint64_t lateralVerticalEntries = 0;
		uint64_t lateralGravityEntries = 0;
		uint64_t lateralSearchSteps = 0;
		uint64_t elementUpdateCalls = 0;
		uint64_t entryMovementClassChanges = 0;
		uint64_t candidateToFluidChanges = 0;
		std::array<uint64_t, ParticleCostClassCount> classCount = {};
		std::array<uint64_t, ParticleCostClassCount> classNs = {};
		std::array<uint64_t, ParticleCostClassCount> movementCalls = {};
		std::array<uint64_t, ParticleCostClassCount> sampledMovementCalls = {};
		std::array<uint64_t, ParticleCostClassCount> sampledMovementRawNs = {};

		void Add(const ParticleCostFrame &other)
		{
			updateCalls += other.updateCalls;
			updateNs += other.updateNs;
			loopNs += other.loopNs;
			slotsSeen += other.slotsSeen;
			liveSeen += other.liveSeen;
			deadSlots += other.deadSlots;
			classSwitches += other.classSwitches;
			tryMoveCalls += other.tryMoveCalls;
			doMoveCalls += other.doMoveCalls;
			lateralVerticalEntries += other.lateralVerticalEntries;
			lateralGravityEntries += other.lateralGravityEntries;
			lateralSearchSteps += other.lateralSearchSteps;
			elementUpdateCalls += other.elementUpdateCalls;
			entryMovementClassChanges += other.entryMovementClassChanges;
			candidateToFluidChanges += other.candidateToFluidChanges;
			for (size_t i = 0; i < ParticleCostClassCount; ++i)
			{
				classCount[i] += other.classCount[i];
				classNs[i] += other.classNs[i];
				movementCalls[i] += other.movementCalls[i];
				sampledMovementCalls[i] += other.sampledMovementCalls[i];
				sampledMovementRawNs[i] += other.sampledMovementRawNs[i];
			}
		}
	};

	std::FILE *particleCostFile = nullptr;
	struct ParticleCostFileLifetime
	{
		~ParticleCostFileLifetime()
		{
			if (particleCostFile)
			{
				std::fclose(particleCostFile);
				particleCostFile = nullptr;
			}
		}
	};
	ParticleCostFileLifetime particleCostFileLifetime;
	bool particleCostChecked = false;
	int particleCostFrameCounter = 0;
	int particleCostSampleStride = 32;
	uint64_t particleCostClockMinNs = 0;
	uint64_t particleCostClockP50Ns = 0;
	double particleCostClockBatchMeanMedianNs = 0.0;
	constexpr int ParticleCostClockCalibrationBatches = 64;
	constexpr int ParticleCostClockCalibrationSamplesPerBatch = 2048;
	ParticleCostFrame particleCostWorking;
	ParticleCostFrame particleCostPending;
	bool particleCostPendingReady = false;
	bool particleCostCollecting = false;
	Simulation *particleCostOwner = nullptr;

	void ClearParticleCostState()
	{
		particleCostWorking = {};
		particleCostPending = {};
		particleCostPendingReady = false;
		particleCostCollecting = false;
	}

	void ReleaseParticleCostOwner(const Simulation *owner)
	{
		if (particleCostOwner == owner)
		{
			ClearParticleCostState();
			particleCostOwner = nullptr;
		}
	}

	void AcquireParticleCostOwner(Simulation *owner)
	{
		if (particleCostOwner != owner)
		{
			ClearParticleCostState();
			particleCostOwner = owner;
		}
	}

	// * Contadores cumulativos de TROCA de posicao (o bloco final de try_move, onde a
	//   particula deslocada e reposicionada em parts[i].x/y). Sao cumulativos de proposito:
	//   a classificacao compara com o valor guardado no frame anterior, e a diferenca da
	//   quantas trocas aquela particula sofreu no frame. Hipotese a testar: um mesmo OIL
	//   trocado varias vezes por vizinhos mais densos acumula deslocamento muito acima do
	//   que qualquer passo unico explicaria.
	std::vector<long long> swapCumCount;
	std::vector<float> swapCumDist;
	bool swapProbeEnabled = false;
}

namespace
{
	struct SimulationImpl : public Simulation
	{
		struct Neighbourhood
		{
			std::array<int, 8> surround;
			int surround_space = 0;
			int nt = 0; //if nt is greater than 1 after this, then there is a particle around the current particle, that is NOT the current particle's type, for water movement.
			float pGravX = 0;
			float pGravY = 0;
		};
		void MovementPhase(int i, Neighbourhood neighbourhood);
		Neighbourhood GetNeighbourhood(int i) const;
		bool TransitionPhase(int i, const Neighbourhood &neighbourhood);

		void UpdateParticles(int start, int end) final override;
	};
}

static float remainder_p(float x, float y)
{
	return std::fmod(x, y) + (x>=0 ? 0 : y);
}

void Simulation::Load(const GameSave *save, bool includePressure, Vec2<int> blockP) // block coordinates
{
	auto partP = blockP * CELL;

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;

	RecalcFreeParticles(false);

	struct ExistingParticle
	{
		int id;
		Vec2<int> pos;
	};
	std::vector<ExistingParticle> existingParticles;
	auto pasteArea = RES.OriginRect() & RectSized(partP, save->blockSize * CELL);
	for (int i = 0; i < parts.active; i++)
	{
		if (parts[i].type)
		{
			auto p = Vec2<int>{ int(parts[i].x + 0.5f), int(parts[i].y + 0.5f) };
			if (pasteArea.Contains(p))
			{
				existingParticles.push_back({ i, p });
			}
		}
	}
	std::sort(existingParticles.begin(), existingParticles.end(), [](const auto &lhs, const auto &rhs) {
		return std::tie(lhs.pos.Y, lhs.pos.X) < std::tie(rhs.pos.Y, rhs.pos.X);
	});
	PlaneAdapter<std::vector<size_t>> existingParticleIndices(pasteArea.size, existingParticles.size());
	{
		auto lastPos = Vec2<int>{ -1, -1 }; // not a valid pos in existingParticles
		for (auto it = existingParticles.begin(); it != existingParticles.end(); ++it)
		{
			if (lastPos != it->pos)
			{
				existingParticleIndices[it->pos - pasteArea.pos] = it - existingParticles.begin();
				lastPos = it->pos;
			}
		}
	}
	auto removeExistingParticles = [this, pasteArea, &existingParticles, &existingParticleIndices](Vec2<int> p) {
		auto rp = p - pasteArea.pos;
		if (existingParticleIndices.Size().OriginRect().Contains(rp))
		{
			auto index = existingParticleIndices[rp];
			for (auto it = existingParticles.begin() + index; it != existingParticles.end() && it->pos == p; ++it)
			{
				kill_part(it->id);
			}
			existingParticleIndices[rp] = existingParticles.size();
		}
	};

	auto oldPrettyPowders = pretty_powder;
	pretty_powder = false;
	Defer restorePrettyPowders([this, oldPrettyPowders]() {
		pretty_powder = oldPrettyPowders;
	});
	std::map<unsigned int, unsigned int> soapList;
	for (int n = 0; n < NPART && n < save->particlesCount; n++)
	{
		Particle tempPart = save->particles[n];
		if (tempPart.type <= 0 || tempPart.type >= PT_NUM)
		{
			continue;
		}

		tempPart.x += (float)partP.X;
		tempPart.y += (float)partP.Y;
		int x = int(std::floor(tempPart.x + 0.5f));
		int y = int(std::floor(tempPart.y + 0.5f));

		// Check various scenarios where we are unable to spawn the element, and set type to 0 to block spawning later
		if (!InBounds(x, y))
		{
			continue;
		}

		// Ensure we can spawn this element
		if ((player.spwn == 1 && tempPart.type==PT_STKM) || (player2.spwn == 1 && tempPart.type==PT_STKM2))
		{
			continue;
		}
		if ((tempPart.type == PT_SPAWN || tempPart.type == PT_SPAWN2) && elementCount[tempPart.type])
		{
			continue;
		}
		if (tempPart.type == PT_FIGH && !Element_FIGH_CanAlloc(this))
		{
			continue;
		}
		if (!elements[tempPart.type].Enabled)
		{
			continue;
		}

		if (elements[tempPart.type].CreateAllowed)
		{
			if (!(*(elements[tempPart.type].CreateAllowed))(this, -3, int(tempPart.x + 0.5f), int(tempPart.y + 0.5f), tempPart.type))
			{
				continue;
			}
		}

		removeExistingParticles({ x, y });

		// Allocate particle (this location is guaranteed to be empty due to "full scan" logic above)
		auto i = create_part(-3, x, y, tempPart.type);
		if (i == -1)
		{
			continue;
		}
		parts[i] = tempPart;


		switch (parts[i].type)
		{
		case PT_STKM:
			Element_STKM_init_legs(this, &player, i);
			player.spwn = 1;
			player.elem = PT_DUST;

			if ((save->version < Version(93, 0) && parts[i].ctype == SPC_AIR) ||
			        (save->version < Version(88, 0) && parts[i].ctype == OLD_SPC_AIR))
			{
				player.fan = true;
			}
			if (save->stkm.rocketBoots1)
				player.rocketBoots = true;
			if (save->stkm.fan1)
				player.fan = true;
			break;
		case PT_STKM2:
			Element_STKM_init_legs(this, &player2, i);
			player2.spwn = 1;
			player2.elem = PT_DUST;
			if ((save->version < Version(93, 0) && parts[i].ctype == SPC_AIR) ||
			        (save->version < Version(88, 0) && parts[i].ctype == OLD_SPC_AIR))
			{
				player2.fan = true;
			}
			if (save->stkm.rocketBoots2)
				player2.rocketBoots = true;
			if (save->stkm.fan2)
				player2.fan = true;
			break;
		case PT_SPAWN:
			player.spawnID = i;
			break;
		case PT_SPAWN2:
			player2.spawnID = i;
			break;
		case PT_FIGH:
		{
			unsigned int oldTmp = parts[i].tmp;
			parts[i].tmp = Element_FIGH_Alloc(this);
			if (parts[i].tmp >= 0)
			{
				bool fan = false;
				if ((save->version < Version(93, 0) && parts[i].ctype == SPC_AIR)
						|| (save->version < Version(88, 0) && parts[i].ctype == OLD_SPC_AIR))
				{
					fan = true;
					parts[i].ctype = 0;
				}
				fighters[parts[i].tmp].elem = PT_DUST;
				Element_FIGH_NewFighter(this, parts[i].tmp, i, parts[i].ctype);
				if (fan)
					fighters[parts[i].tmp].fan = true;
				for (unsigned int fighNum : save->stkm.rocketBootsFigh)
				{
					if (fighNum == oldTmp)
						fighters[parts[i].tmp].rocketBoots = true;
				}
				for (unsigned int fighNum : save->stkm.fanFigh)
				{
					if (fighNum == oldTmp)
						fighters[parts[i].tmp].fan = true;
				}
			}
			else
			{
				// Should not be possible because we verify with CanAlloc above this
				parts[i].type = 0;
			}
			break;
		}
		case PT_SOAP:
			soapList.insert(std::pair<unsigned int, unsigned int>(n, i));
			break;
		}
		if (GameSave::PressureInTmp3(parts[i].type) && !includePressure)
		{
			parts[i].tmp3 = 0;
		}
	}
	parts.active = NPART;
	force_stacking_check = true;
	Element_PPIP_ppip_changed = 1;

	// Sort out pmap, just to be on the safe side.
	RecalcFreeParticles(false);

	// fix SOAP links using soapList, a map of old particle ID -> new particle ID
	// loop through every old particle (loaded from save), and convert .tmp / .tmp2
	for (std::map<unsigned int, unsigned int>::iterator iter = soapList.begin(), end = soapList.end(); iter != end; ++iter)
	{
		int i = (*iter).second;
		if ((parts[i].ctype & 0x2) == 2)
		{
			std::map<unsigned int, unsigned int>::iterator n = soapList.find(parts[i].tmp);
			if (n != end)
				parts[i].tmp = n->second;
			// sometimes the proper SOAP isn't found. It should remove the link, but seems to break some saves
			// so just ignore it
		}
		if ((parts[i].ctype & 0x4) == 4)
		{
			std::map<unsigned int, unsigned int>::iterator n = soapList.find(parts[i].tmp2);
			if (n != end)
				parts[i].tmp2 = n->second;
			// sometimes the proper SOAP isn't found. It should remove the link, but seems to break some saves
			// so just ignore it
		}
	}

	for (size_t i = 0; i < save->signs.size() && signs.size() < MAXSIGNS; i++)
	{
		if (save->signs[i].text.length())
		{
			sign tempSign = save->signs[i];
			tempSign.x += partP.X;
			tempSign.y += partP.Y;
			if (!InBounds(tempSign.x, tempSign.y))
			{
				continue;
			}
			signs.push_back(tempSign);
		}
	}
	auto useGravityMaps = save->hasGravityMaps && grav;
	auto targetBlocks = RectSized(blockP + save->blockContent.TopLeft(), save->blockContent.size) & CELLS.OriginRect();
	for (auto bpos : targetBlocks)
	{
		auto spos = bpos - blockP;
		if (save->blockMap[spos])
		{
			bmap[bpos.Y][bpos.X] = save->blockMap[spos];
			fvx[bpos.Y][bpos.X] = save->fanVelX[spos];
			fvy[bpos.Y][bpos.X] = save->fanVelY[spos];
		}
		if (includePressure)
		{
			if (save->hasPressure)
			{
				pv[bpos.Y][bpos.X] = save->pressure[spos];
				vx[bpos.Y][bpos.X] = save->velocityX[spos];
				vy[bpos.Y][bpos.X] = save->velocityY[spos];
			}
			if (save->hasAmbientHeat)
			{
				hv[bpos.Y][bpos.X] = save->ambientHeat[spos];
			}
			if (save->hasBlockAirMaps)
			{
				air->bmap_blockair [bpos.Y][bpos.X] = save->blockAir [spos];
				air->bmap_blockairh[bpos.Y][bpos.X] = save->blockAirh[spos];
			}
		}
		if (useGravityMaps)
		{
			gravIn.mass   [bpos] = save->gravMass  [spos];
			gravIn.mask   [bpos] = save->gravMask  [spos];
			gravOut.forceX[bpos] = save->gravForceX[spos];
			gravOut.forceY[bpos] = save->gravForceY[spos];
			gravForceRecalc = true; // gravOut changed outside DispatchNewtonianGravity
		}
	}
	if (useGravityMaps)
	{
		ResetNewtonianGravity(gravIn, gravOut);
	}

	gravWallChanged = true;
	if (!save->hasBlockAirMaps)
	{
		air->ApproximateBlockAirMaps(targetBlocks);
	}
}

std::unique_ptr<GameSave> Simulation::Save(bool includePressure, Rect<int> partR) // particle coordinates
{
	auto blockR = RectBetween(partR.TopLeft() / CELL, partR.BottomRight() / CELL);
	auto blockP = blockR.pos;

	auto newSave = std::make_unique<GameSave>(blockR.size);
	newSave->frameCount = frameCount;
	newSave->rngState = rng.state();

	int storedParts = 0;
	int elementCount[PT_NUM];
	std::fill(elementCount, elementCount+PT_NUM, 0);
	// Map of soap particles loaded into this save, old ID -> new ID
	// Now stores all particles, not just SOAP (but still only used for soap)
	std::map<unsigned int, unsigned int> particleMap;

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	for (int i = 0; i < NPART; i++)
	{
		int x, y;
		x = int(parts[i].x + 0.5f);
		y = int(parts[i].y + 0.5f);
		if (parts[i].type && partR.Contains({ x, y }))
		{
			Particle tempPart = parts[i];
			tempPart.x -= blockP.X * CELL;
			tempPart.y -= blockP.Y * CELL;
			if (elements[tempPart.type].Enabled)
			{
				particleMap.insert(std::pair<unsigned int, unsigned int>(i, storedParts));
				*newSave << tempPart;
				storedParts++;
				elementCount[tempPart.type]++;

			}
		}
	}

	if (storedParts && elementCount[PT_SOAP])
	{
		// fix SOAP links using particleMap, a map of old particle ID -> new particle ID
		// loop through every new particle (saved into the save), and convert .tmp / .tmp2
		for (std::map<unsigned int, unsigned int>::iterator iter = particleMap.begin(), end = particleMap.end(); iter != end; ++iter)
		{
			int i = (*iter).second;
			if (newSave->particles[i].type != PT_SOAP)
				continue;
			if ((newSave->particles[i].ctype & 0x2) == 2)
			{
				std::map<unsigned int, unsigned int>::iterator n = particleMap.find(newSave->particles[i].tmp);
				if (n != end)
					newSave->particles[i].tmp = n->second;
				else
				{
					newSave->particles[i].tmp = 0;
					newSave->particles[i].ctype ^= 2;
				}
			}
			if ((newSave->particles[i].ctype & 0x4) == 4)
			{
				std::map<unsigned int, unsigned int>::iterator n = particleMap.find(newSave->particles[i].tmp2);
				if (n != end)
					newSave->particles[i].tmp2 = n->second;
				else
				{
					newSave->particles[i].tmp2 = 0;
					newSave->particles[i].ctype ^= 4;
				}
			}
		}
	}

	for (size_t i = 0; i < MAXSIGNS && i < signs.size(); i++)
	{
		if (signs[i].text.length() && partR.Contains({ signs[i].x, signs[i].y }))
		{
			sign tempSign = signs[i];
			tempSign.x -= blockP.X * CELL;
			tempSign.y -= blockP.Y * CELL;
			*newSave << tempSign;
		}
	}

	for (auto bpos : newSave->blockSize.OriginRect())
	{
		if(bmap[bpos.Y + blockP.Y][bpos.X + blockP.X])
		{
			newSave->blockMap[bpos] = bmap[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->fanVelX[bpos] = fvx[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->fanVelY[bpos] = fvy[bpos.Y + blockP.Y][bpos.X + blockP.X];
		}
		if (includePressure)
		{
			newSave->pressure[bpos] = pv[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->velocityX[bpos] = vx[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->velocityY[bpos] = vy[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->ambientHeat[bpos] = hv[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->blockAir[bpos] = air->bmap_blockair[bpos.Y + blockP.Y][bpos.X + blockP.X];
			newSave->blockAirh[bpos] = air->bmap_blockairh[bpos.Y + blockP.Y][bpos.X + blockP.X];
		}
		if (grav)
		{
			newSave->gravMass  [bpos] = gravIn.mass   [bpos + blockP];
			newSave->gravMask  [bpos] = gravIn.mask   [bpos + blockP];
			newSave->gravForceX[bpos] = gravOut.forceX[bpos + blockP];
			newSave->gravForceY[bpos] = gravOut.forceY[bpos + blockP];
		}
	}
	if (includePressure)
	{
		newSave->hasBlockAirMaps = true;
	}
	if (grav)
	{
		newSave->hasGravityMaps = true;
	}
	if (includePressure || ensureDeterminism)
	{
		newSave->hasPressure = true;
		newSave->hasAmbientHeat = true;
	}
	newSave->ensureDeterminism = ensureDeterminism;

	newSave->stkm.rocketBoots1 = player.rocketBoots;
	newSave->stkm.rocketBoots2 = player2.rocketBoots;
	newSave->stkm.fan1 = player.fan;
	newSave->stkm.fan2 = player2.fan;
	for (unsigned char i = 0; i < MAX_FIGHTERS; i++)
	{
		if (fighters[i].rocketBoots)
			newSave->stkm.rocketBootsFigh.push_back(i);
		if (fighters[i].fan)
			newSave->stkm.fanFigh.push_back(i);
	}

	SaveSimOptions(*newSave);
	newSave->pmapbits = PMAPBITS;
	return newSave;
}

void Simulation::SaveSimOptions(GameSave &gameSave)
{
	gameSave.gravityMode = gravityMode;
	gameSave.customGravityX = customGravityX;
	gameSave.customGravityY = customGravityY;
	gameSave.airMode = air->airMode;
	gameSave.ambientAirTemp = air->ambientAirTemp;
	gameSave.edgePressure = air->edgePressure;
	gameSave.edgeVelocityX = air->edgeVelocityX;
	gameSave.edgeVelocityY = air->edgeVelocityY;
	gameSave.vorticityCoeff = air->vorticityCoeff;
	gameSave.convectionMode = air->convectionMode;
	gameSave.edgeMode = edgeMode;
	gameSave.legacyEnable = legacy_enable;
	gameSave.waterEEnabled = water_equal_test;
	gameSave.gravityEnable = bool(grav);
	gameSave.aheatEnable = aheat_enable;
}

bool Simulation::FloodFillPmapCheck(int x, int y, int type) const
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	if (type == 0)
		return !pmap[y][x] && !photons[y][x];
	if (elements[type].Properties&TYPE_ENERGY)
		return TYP(photons[y][x]) == type;
	else
		return TYP(pmap[y][x]) == type;
}

CoordStack& Simulation::getCoordStackSingleton()
{
	// Future-proofing in case Simulation is later multithreaded
	thread_local CoordStack cs;
	return cs;
}

int Simulation::flood_prop(int x, int y, const AccessProperty &changeProperty)
{
	int i, x1, x2, dy = 1;
	int did_something = 0;
	int r = pmap[y][x];
	if (!r)
		r = photons[y][x];
	if (!r)
		return 0;
	int parttype = TYP(r);
	char * bitmap = (char*)malloc(XRES*YRES); //Bitmap for checking
	if (!bitmap) return -1;
	memset(bitmap, 0, XRES*YRES);
	try
	{
		CoordStack& cs = getCoordStackSingleton();
		cs.clear();

		cs.push(x, y);
		do
		{
			cs.pop(x, y);
			x1 = x2 = x;
			while (x1>=CELL)
			{
				if (!FloodFillPmapCheck(x1-1, y, parttype) || bitmap[(y*XRES)+x1-1])
					break;
				x1--;
			}
			while (x2<XRES-CELL)
			{
				if (!FloodFillPmapCheck(x2+1, y, parttype) || bitmap[(y*XRES)+x2+1])
					break;
				x2++;
			}
			for (x=x1; x<=x2; x++)
			{
				i = pmap[y][x];
				if (!i)
					i = photons[y][x];
				if (!i)
					continue;
				changeProperty.Set(this, ID(i));
				bitmap[(y*XRES)+x] = 1;
				did_something = 1;
			}
			if (y>=CELL+dy)
				for (x=x1; x<=x2; x++)
					if (FloodFillPmapCheck(x, y-dy, parttype) && !bitmap[((y-dy)*XRES)+x])
						cs.push(x, y-dy);
			if (y<YRES-CELL-dy)
				for (x=x1; x<=x2; x++)
					if (FloodFillPmapCheck(x, y+dy, parttype) && !bitmap[((y+dy)*XRES)+x])
						cs.push(x, y+dy);
		} while (cs.getSize()>0);
	}
	catch (std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		free(bitmap);
		return -1;
	}
	free(bitmap);
	return did_something;
}

int Simulation::FloodINST(int x, int y)
{
	int x1, x2;
	int created_something = 0;

	const auto isSparkableInst = [this](int x, int y) -> bool {
		return TYP(pmap[y][x])==PT_INST && parts[ID(pmap[y][x])].life==0;
	};

	const auto isInst = [this](int x, int y) -> bool {
		return TYP(pmap[y][x])==PT_INST || (TYP(pmap[y][x])==PT_SPRK  && parts[ID(pmap[y][x])].ctype==PT_INST);
	};

	if (!isSparkableInst(x,y))
		return 1;

	CoordStack& cs = getCoordStackSingleton();
	cs.clear();

	cs.push(x, y);

	try
	{
		do
		{
			cs.pop(x, y);
			x1 = x2 = x;
			// go left as far as possible
			while (x1>=CELL && isSparkableInst(x1-1, y))
			{
				x1--;
			}
			// go right as far as possible
			while (x2<XRES-CELL && isSparkableInst(x2+1, y))
			{
				x2++;
			}
			// fill span
			for (x=x1; x<=x2; x++)
			{
				if (create_part(-1, x, y, PT_SPRK)>=0)
					created_something = 1;
			}

			// add vertically adjacent pixels to stack
			// (wire crossing for INST)
			if (y>=CELL+1 && x1==x2 &&
				isInst(x1-1, y-1) && isInst(x1, y-1) && isInst(x1+1, y-1) &&
				!isInst(x1-1, y-2) && isInst(x1, y-2) && !isInst(x1+1, y-2))
			{
				// travelling vertically up, skipping a horizontal line
				if (isSparkableInst(x1, y-2))
				{
						cs.push(x1, y-2);
				}
			}
			else if (y>=CELL+1)
			{
				for (x=x1; x<=x2; x++)
				{
					if (isSparkableInst(x, y-1))
					{
						if (x==x1 || x==x2 || y>=YRES-CELL-1 || !isInst(x, y+1) || isInst(x+1, y+1) || isInst(x-1, y+1))
						{
							// if at the end of a horizontal section, or if it's a T junction or not a 1px wire crossing
							cs.push(x, y-1);
						}
					}
				}
			}

			if (y<YRES-CELL-1 && x1==x2 &&
				isInst(x1-1, y+1) && isInst(x1, y+1) && isInst(x1+1, y+1) &&
				!isInst(x1-1, y+2) && isInst(x1, y+2) && !isInst(x1+1, y+2))
			{
				// travelling vertically down, skipping a horizontal line
				if (isSparkableInst(x1, y+2))
				{
					cs.push(x1, y+2);
				}
			}
			else if (y<YRES-CELL-1)
			{
				for (x=x1; x<=x2; x++)
				{
					if (isSparkableInst(x, y+1))
					{
						if (x==x1 || x==x2 || y<0 || !isInst(x, y-1) || isInst(x+1, y-1) || isInst(x-1, y-1))
						{
							// if at the end of a horizontal section, or if it's a T junction or not a 1px wire crossing
							cs.push(x, y+1);
						}

					}
				}
			}
		} while (cs.getSize()>0);
	}
	catch (std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return -1;
	}

	return created_something;
}

bool Simulation::flood_water(int x, int y, int i)
{
	int x1, x2, originalX = x, originalY = y;
	int r = pmap[y][x];
	if (!r)
		return false;

	// Bitmap for checking where we've already looked
	auto bitmapPtr = std::unique_ptr<char[]>(new char[XRES * YRES]);
	char *bitmap = bitmapPtr.get();
	std::fill(&bitmap[0], &bitmap[0] + XRES * YRES, 0);

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	try
	{
		CoordStack& cs = getCoordStackSingleton();
		cs.clear();

		cs.push(x, y);
		do
		{
			cs.pop(x, y);
			x1 = x2 = x;
			while (x1 >= CELL)
			{
				if (elements[TYP(pmap[y][x1 - 1])].Falldown != 2 || bitmap[(y * XRES) + x1 - 1])
					break;
				x1--;
			}
			while (x2 < XRES-CELL)
			{
				if (elements[TYP(pmap[y][x2 + 1])].Falldown != 2 || bitmap[(y * XRES) + x1 - 1])
					break;
				x2++;
			}
			for (int x = x1; x <= x2; x++)
			{
				if ((y - 1) > originalY && !pmap[y - 1][x])
				{
					// Try to move the water to a random position on this line, because there's probably a free location somewhere
					int randPos = rng.between(x, x2);
					if (!pmap[y - 1][randPos] && eval_move(parts[i].type, randPos, y - 1, nullptr))
						x = randPos;
					// Couldn't move to random position, so try the original position on the left
					else if (!eval_move(parts[i].type, x, y - 1, nullptr))
						continue;

					move(i, originalX, originalY, float(x), float(y - 1));
					return true;
				}

				bitmap[(y * XRES) + x] = 1;
			}
			if (y >= CELL + 1)
				for (int x = x1; x <= x2; x++)
					if (elements[TYP(pmap[y - 1][x])].Falldown == 2 && !bitmap[((y - 1) * XRES) + x])
						cs.push(x, y - 1);
			if (y < YRES - CELL - 1)
				for (int x = x1; x <= x2; x++)
					if (elements[TYP(pmap[y + 1][x])].Falldown == 2 && !bitmap[((y + 1) * XRES) + x])
						cs.push(x, y + 1);
		} while (cs.getSize() > 0);
	}
	catch (std::exception &e)
	{
		std::cerr << e.what() << std::endl;
		return false;
	}
	return false;
}

void Simulation::SetEdgeMode(int newEdgeMode)
{
	edgeMode = newEdgeMode;
	switch(edgeMode)
	{
	case EDGE_VOID:
	case EDGE_LOOP:
		for(int i = 0; i<XCELLS; i++)
		{
			bmap[0][i] = 0;
			bmap[YCELLS-1][i] = 0;
		}
		for(int i = 1; i<(YCELLS-1); i++)
		{
			bmap[i][0] = 0;
			bmap[i][XCELLS-1] = 0;
		}
		break;
	case EDGE_SOLID:
		int i;
		for(i=0; i<XCELLS; i++)
		{
			bmap[0][i] = WL_WALL;
			bmap[YCELLS-1][i] = WL_WALL;
		}
		for(i=1; i<(YCELLS-1); i++)
		{
			bmap[i][0] = WL_WALL;
			bmap[i][XCELLS-1] = WL_WALL;
		}
		break;
	default:
		SetEdgeMode(EDGE_VOID);
	}
}

// Now simply creates a 0 pixel radius line without all the complicated flags / other checks
// Would make sense to move to Editing.cpp but SPRK needs it.
void Simulation::CreateLine(int x1, int y1, int x2, int y2, int c)
{
	bool reverseXY = abs(y2-y1) > abs(x2-x1);
	int x, y, dx, dy, sy;
	float e, de;
	int v = ID(c);
	c = TYP(c);
	if (reverseXY)
	{
		y = x1;
		x1 = y1;
		y1 = y;
		y = x2;
		x2 = y2;
		y2 = y;
	}
	if (x1 > x2)
	{
		y = x1;
		x1 = x2;
		x2 = y;
		y = y1;
		y1 = y2;
		y2 = y;
	}
	dx = x2 - x1;
	dy = abs(y2 - y1);
	e = 0.0f;
	de = dx ? dy/(float)dx : 0.0f;
	y = y1;
	sy = (y1<y2) ? 1 : -1;
	for (x=x1; x<=x2; x++)
	{
		if (reverseXY)
			create_part(-1, y, x, c, v);
		else
			create_part(-1, x, y, c, v);
		e += de;
		if (e >= 0.5f)
		{
			y += sy;
			if ((y1<y2) ? (y<=y2) : (y>=y2))
			{
				if (reverseXY)
					create_part(-1, y, x, c, v);
				else
					create_part(-1, x, y, c, v);
			}
			e -= 1.0f;
		}
	}
}

inline int Simulation::is_wire(int x, int y)
{
	return bmap[y][x]==WL_DETECT || bmap[y][x]==WL_EWALL || bmap[y][x]==WL_ALLOWLIQUID || bmap[y][x]==WL_WALLELEC || bmap[y][x]==WL_ALLOWALLELEC || bmap[y][x]==WL_EHOLE || bmap[y][x]==WL_STASIS;
}

inline int Simulation::is_wire_off(int x, int y)
{
	return (bmap[y][x]==WL_DETECT || bmap[y][x]==WL_EWALL || bmap[y][x]==WL_ALLOWLIQUID || bmap[y][x]==WL_WALLELEC || bmap[y][x]==WL_ALLOWALLELEC || bmap[y][x]==WL_EHOLE || bmap[y][x]==WL_STASIS) && emap[y][x]<8;
}

// implement __builtin_ctz and __builtin_clz on msvc
#ifdef _MSC_VER
unsigned msvc_ctz(unsigned a)
{
	unsigned long i;
	_BitScanForward(&i, a);
	return i;
}

unsigned msvc_clz(unsigned a)
{
	unsigned long i;
	_BitScanReverse(&i, a);
	return 31 - i;
}

#define __builtin_ctz msvc_ctz
#define __builtin_clz msvc_clz
#endif

int Simulation::get_wavelength_bin(int *wm)
{
	int i, w0, wM, r;

	if (!(*wm & 0x3FFFFFFF))
		return -1;

#if defined(__GNUC__) || defined(_MSVC_VER)
	w0 = __builtin_ctz(*wm | 0xC0000000);
	wM = 31 - __builtin_clz(*wm & 0x3FFFFFFF);
#else
	w0 = 30;
	wM = 0;
	for (i = 0; i < 30; i++)
		if (*wm & (1<<i))
		{
			if (i < w0)
				w0 = i;
			if (i > wM)
				wM = i;
		}
#endif

	if (wM - w0 < 5)
		return wM + w0;

	r = rng.gen();
	i = (r >> 1) % (wM-w0-4);
	i += w0;

	if (r & 1)
	{
		*wm &= 0x1F << i;
		return (i + 2) * 2;
	}
	else
	{
		*wm &= 0xF << i;
		return (i + 2) * 2 - 1;
	}
}

void Simulation::set_emap(int x, int y)
{
	int x1, x2;

	if (!is_wire_off(x, y))
		return;

	// go left as far as possible
	x1 = x2 = x;
	while (x1>0)
	{
		if (!is_wire_off(x1-1, y))
			break;
		x1--;
	}
	while (x2<XCELLS-1)
	{
		if (!is_wire_off(x2+1, y))
			break;
		x2++;
	}

	// fill span
	for (x=x1; x<=x2; x++)
		emap[y][x] = 16;

	// fill children

	if (y>1 && x1==x2 &&
	        is_wire(x1-1, y-1) && is_wire(x1, y-1) && is_wire(x1+1, y-1) &&
	        !is_wire(x1-1, y-2) && is_wire(x1, y-2) && !is_wire(x1+1, y-2))
		set_emap(x1, y-2);
	else if (y>0)
		for (x=x1; x<=x2; x++)
			if (is_wire_off(x, y-1))
			{
				if (x==x1 || x==x2 || y>=YCELLS-1 ||
				        is_wire(x-1, y-1) || is_wire(x+1, y-1) ||
				        is_wire(x-1, y+1) || !is_wire(x, y+1) || is_wire(x+1, y+1))
					set_emap(x, y-1);
			}

	if (y<YCELLS-2 && x1==x2 &&
	        is_wire(x1-1, y+1) && is_wire(x1, y+1) && is_wire(x1+1, y+1) &&
	        !is_wire(x1-1, y+2) && is_wire(x1, y+2) && !is_wire(x1+1, y+2))
		set_emap(x1, y+2);
	else if (y<YCELLS-1)
		for (x=x1; x<=x2; x++)
			if (is_wire_off(x, y+1))
			{
				if (x==x1 || x==x2 || y<0 ||
				        is_wire(x-1, y+1) || is_wire(x+1, y+1) ||
				        is_wire(x-1, y-1) || !is_wire(x, y-1) || is_wire(x+1, y-1))
					set_emap(x, y+1);
			}
}

int Simulation::parts_avg(int ci, int ni,int t)
{
	if (t==PT_INSL)//to keep electronics working
	{
		int pmr = pmap[((int)(parts[ci].y+0.5f) + (int)(parts[ni].y+0.5f))/2][((int)(parts[ci].x+0.5f) + (int)(parts[ni].x+0.5f))/2];
		if (pmr)
			return parts[ID(pmr)].type;
		else
			return PT_NONE;
	}
	else
	{
		int pmr2 = pmap[(int)((parts[ci].y + parts[ni].y)/2+0.5f)][(int)((parts[ci].x + parts[ni].x)/2+0.5f)];//seems to be more accurate.
		if (pmr2)
		{
			if (parts[ID(pmr2)].type==t)
				return t;
		}
		else
			return PT_NONE;
	}
	return PT_NONE;
}

void Parts::Reset()
{
	memset(data.data(), 0, sizeof(Particle)*NPART);
	active = 0;
	pfree.fill(-1);
	currentSegment = 0;
	// * Numero de segmentos por ambiente. Resolvido uma vez; o padrao 1 reproduz exatamente
	//   a lista unica original, entao jogo normal nao muda ate alguem pedir o contrario.
	static const int segmentsFromEnv = []() {
		if (auto *env = std::getenv("TPT_FREELIST_SEGMENTS"))
		{
			return std::atoi(env);
		}
		return 1;
	}();
	SetFreeSegments(segmentsFromEnv);
}

void Parts::SetFreeSegments(int count)
{
	freeSegments = std::max(1, std::min(count, MaxFreeSegments));
}

bool Parts::ValidateFreeLists(int &freeCount) const
{
	std::vector<char> seen(NPART, 0);
	freeCount = 0;
	for (auto segment = 0; segment < freeSegments; segment++)
	{
		auto guard = 0;
		for (auto i = pfree[segment]; i != -1; i = data[i].life)
		{
			// * Indice fora de faixa, slot ja visto (duplicata entre listas ou ciclo) ou
			//   slot com tipo vivo dentro da lista livre sao todos corrupcao.
			if (i < 0 || i >= NPART || seen[i] || data[i].type)
			{
				return false;
			}
			seen[i] = 1;
			freeCount += 1;
			if (++guard > NPART)
			{
				return false;
			}
		}
	}
	return true;
}

void Simulation::clear_sim(void)
{
	// A debug partial update may not reach AfterSim. Do not carry its probe sample into a
	// newly loaded or cleared simulation.
	ReleaseParticleCostOwner(this);
	for (auto i = 0; i < parts.active; i++)
	{
		if (parts[i].type)
		{
			kill_part(i);
		}
	}
	ensureDeterminism = false;
	frameCount = 0;
	debug_nextToUpdate = 0;
	debug_mostRecentlyUpdated = -1;
	emp_decor = 0;
	emp_trigger_count = 0;
	signs.clear();
	memset(bmap, 0, sizeof(bmap));
	memset(emap, 0, sizeof(emap));
	parts.Reset();
	NUM_PARTS = 0;
	memset(pmap, 0, sizeof(pmap));
	memset(fvx, 0, sizeof(fvx));
	memset(fvy, 0, sizeof(fvy));
	memset(photons, 0, sizeof(photons));
	memset(wireless, 0, sizeof(wireless));
	memset(gol, 0, sizeof(gol));
	memset(portalp, 0, sizeof(portalp));
	memset(fighters, 0, sizeof(fighters));
	memset(&player, 0, sizeof(player));
	memset(&player2, 0, sizeof(player2));
	memset(&Element_LOLZ_lolz, 0, sizeof(Element_LOLZ_lolz));
	memset(&Element_LOVE_love, 0, sizeof(Element_LOVE_love));
	memset(&Element_PSTN_tempParts, 0, sizeof(Element_PSTN_tempParts));
	Element_PPIP_ppip_changed = 0;
	std::fill(elementCount, elementCount+PT_NUM, 0);
	elementRecount = true;
	fighcount = 0;
	player.spwn = 0;
	player.spawnID = -1;
	player.rocketBoots = false;
	player.fan = false;
	player2.spwn = 0;
	player2.spawnID = -1;
	player2.rocketBoots = false;
	player2.fan = false;
	//memset(pers_bg, 0, WINDOWW*YRES*PIXELSIZE);
	//memset(fire_r, 0, sizeof(fire_r));
	//memset(fire_g, 0, sizeof(fire_g));
	//memset(fire_b, 0, sizeof(fire_b));
	//if(gravmask)
		//memset(gravmask, 0xFFFFFFFF, NCELL*sizeof(unsigned));
	ResetNewtonianGravity({}, {});
	if(air)
	{
		air->Clear();
		air->ClearAirH();
	}
	SetEdgeMode(edgeMode);
}

bool Simulation::IsWallBlocking(int x, int y, int type) const
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	if (bmap[y/CELL][x/CELL])
	{
		int wall = bmap[y/CELL][x/CELL];
		if (wall == WL_ALLOWGAS && !(elements[type].Properties&TYPE_GAS))
			return true;
		else if (wall == WL_ALLOWENERGY && !(elements[type].Properties&TYPE_ENERGY))
			return true;
		else if (wall == WL_ALLOWLIQUID && !(elements[type].Properties&TYPE_LIQUID))
			return true;
		else if (wall == WL_ALLOWPOWDER && !(elements[type].Properties&TYPE_PART))
			return true;
		else if (wall == WL_ALLOWAIR || wall == WL_WALL || wall == WL_WALLELEC)
			return true;
		else if (wall == WL_EWALL && !emap[y/CELL][x/CELL])
			return true;
		else if (wall == WL_DETECT && (elements[type].Properties&TYPE_SOLID))
			return true;
	}
	return false;
}

/*
   RETURN-value explanation
1 = Swap
0 = No move/Bounce
2 = Both particles occupy the same space.
 */
int Simulation::eval_move(int pt, int nx, int ny, unsigned *rr) const
{
	unsigned r;
	int result;

	if (nx<0 || ny<0 || nx>=XRES || ny>=YRES)
		return 0;

	r = pmap[ny][nx];
	if (r)
		r = (r&~PMAPMASK) | parts[ID(r)].type;
	if (rr)
		*rr = r;
	if (pt>=PT_NUM || TYP(r)>=PT_NUM)
		return 0;
	auto &sd = SimulationData::CRef();
	auto &can_move = sd.can_move;
	auto &elements = sd.elements;
	result = can_move[pt][TYP(r)];
	if (result == 3)
	{
		switch (TYP(r))
		{
		case PT_LCRY:
			if (pt==PT_PHOT)
				result = (parts[ID(r)].life > 5)? 2 : 0;
			break;
		case PT_GPMP:
			if (pt == PT_PHOT)
				result = (parts[ID(r)].life < 10) ? 2 : 0;
			break;
		case PT_INVIS:
		{
			float pressureResistance = 0.0f;
			if (parts[ID(r)].tmp > 0)
				pressureResistance = (float)parts[ID(r)].tmp;
			else
				pressureResistance = 4.0f;

			if (pv[ny/CELL][nx/CELL] < -pressureResistance || pv[ny/CELL][nx/CELL] > pressureResistance)
				result = 2;
			else
				result = 0;
			break;
		}
		case PT_PVOD:
			if (parts[ID(r)].life == 10)
			{
				if (!parts[ID(r)].ctype || (parts[ID(r)].ctype==pt)!=(parts[ID(r)].tmp&1))
					result = 1;
				else
					result = 0;
			}
			else result = 0;
			break;
		case PT_VOID:
			if (!parts[ID(r)].ctype || (parts[ID(r)].ctype==pt)!=(parts[ID(r)].tmp&1))
				result = 1;
			else
				result = 0;
			break;
		case PT_SWCH:
			if (pt == PT_TRON)
			{
				if (parts[ID(r)].life >= 10)
					return 2;
				else
					return 0;
			}
			break;
		default:
			// This should never happen
			// If it were to happen, try_move would interpret a 3 as a 1
			result =  1;
		}
	}
	if (bmap[ny/CELL][nx/CELL])
	{
		if (IsWallBlocking(nx, ny, pt))
			return 0;
		if (bmap[ny/CELL][nx/CELL]==WL_EHOLE && !emap[ny/CELL][nx/CELL] && !(elements[pt].Properties&TYPE_SOLID) && !(elements[TYP(r)].Properties&TYPE_SOLID))
			return 2;
	}
	return result;
}

int Simulation::try_move(int i, int x, int y, int nx, int ny)
{
	if (particleCostCollecting)
	{
		particleCostWorking.tryMoveCalls += 1;
	}
	unsigned r = 0, e;

	if (x==nx && y==ny)
		return 1;
	if (nx<0 || ny<0 || nx>=XRES || ny>=YRES)
		return 1;

	e = eval_move(parts[i].type, nx, ny, &r);

	/* half-silvered mirror */
	if (!e && parts[i].type==PT_PHOT && ((TYP(r)==PT_BMTL && rng.chance(1, 2)) || TYP(pmap[y][x])==PT_BMTL))
		e = 2;

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	if (!e) //if no movement
	{
		int rt = TYP(r);
		if (rt == PT_WOOD)
		{
			//@ WOOD -> SAWD
			float vel = std::sqrt(std::pow(parts[i].vx, 2) + std::pow(parts[i].vy, 2));
			if (vel > 5)
				part_change_type(ID(r), nx, ny, PT_SAWD);
		}
		if (!(elements[parts[i].type].Properties & TYPE_ENERGY))
			return 0;
		if (!legacy_enable && parts[i].type==PT_PHOT && r)//PHOT heat conduction
		{
			if (rt == PT_COAL || rt == PT_BCOL)
				parts[ID(r)].temp = parts[i].temp;

			if (rt < PT_NUM && !sd.IsHeatInsulator(parts[ID(r)]) && rt != PT_FILT)
				parts[i].temp = parts[ID(r)].temp = restrict_flt((parts[ID(r)].temp+parts[i].temp)/2, MIN_TEMP, MAX_TEMP);
		}
		else if ((parts[i].type==PT_NEUT || parts[i].type==PT_ELEC) && (rt==PT_CLNE || rt==PT_PCLN || rt==PT_BCLN || rt==PT_PBCN))
		{
			if (!parts[ID(r)].ctype)
				parts[ID(r)].ctype = parts[i].type;
		}
		if (rt==PT_PRTI && (elements[parts[i].type].Properties & TYPE_ENERGY))
		{
			int nnx, count;
			for (count=0; count<8; count++)
			{
				if (isign(x-nx)==isign(portal_rx[count]) && isign(y-ny)==isign(portal_ry[count]))
					break;
			}
			count = count%8;
			parts[ID(r)].tmp = (int)((parts[ID(r)].temp-73.15f)/100+1);
			if (parts[ID(r)].tmp>=CHANNELS) parts[ID(r)].tmp = CHANNELS-1;
			else if (parts[ID(r)].tmp<0) parts[ID(r)].tmp = 0;
			for ( nnx=0; nnx<80; nnx++)
				if (!portalp[parts[ID(r)].tmp][count][nnx].type)
				{
					portalp[parts[ID(r)].tmp][count][nnx] = parts[i];
					kill_part(i);
					break;
				}
		}
		return 0;
	}

	if (e == 2) //if occupy same space
	{
		switch (parts[i].type)
		{
		case PT_PHOT:
		{
			switch (TYP(r))
			{
			case PT_GLOW:
				if (!parts[ID(r)].life && rng.chance(1, 30))
				{
					parts[ID(r)].life = 120;
					create_gain_photon(i);
				}
				break;
			case PT_FILT:
				parts[i].ctype = Element_FILT_interactWavelengths(this, &parts[ID(r)], parts[i].ctype);
				break;
			case PT_C5:
				if (parts[ID(r)].life > 0 && (parts[ID(r)].ctype & parts[i].ctype & 0xFFFFFFC0))
				{
					float vx = ((parts[ID(r)].tmp << 16) >> 16) / 255.0f;
					float vy = (parts[ID(r)].tmp >> 16) / 255.0f;
					float vn = parts[i].vx * parts[i].vx + parts[i].vy * parts[i].vy;
					// if the resulting velocity would be 0, that would cause division by 0 inside the else
					// shoot the photon off at a 90 degree angle instead (probably particle order dependent)
					if (parts[i].vx + vx == 0 && parts[i].vy + vy == 0)
					{
						parts[i].vx = vy;
						parts[i].vy = -vx;
					}
					else
					{
						parts[i].ctype = (parts[ID(r)].ctype & parts[i].ctype) >> 6;
						// add momentum of photons to each other
						parts[i].vx += vx;
						parts[i].vy += vy;
						// normalize velocity to original value
						vn /= parts[i].vx * parts[i].vx + parts[i].vy * parts[i].vy;
						vn = sqrtf(vn);
						parts[i].vx *= vn;
						parts[i].vy *= vn;
					}
					parts[ID(r)].life = 0;
					parts[ID(r)].ctype = 0;
				}
				else if(!parts[ID(r)].ctype && parts[i].ctype & 0xFFFFFFC0)
				{
					parts[ID(r)].life = 1;
					parts[ID(r)].ctype = parts[i].ctype;
					parts[ID(r)].tmp = (0xFFFF & (int)(parts[i].vx * 255.0f)) | (0xFFFF0000 & (int)(parts[i].vy * 16711680.0f));
					parts[ID(r)].tmp2 = (0xFFFF & (int)((parts[i].x - x) * 255.0f)) | (0xFFFF0000 & (int)((parts[i].y - y) * 16711680.0f));
					kill_part(i);
				}
				break;
			case PT_INVIS:
			{
				float pressureResistance = 0.0f;
				pressureResistance = (parts[ID(r)].tmp > 0) ? (float)parts[ID(r)].tmp : 4.0f;

				if (pv[ny/CELL][nx/CELL] >= -pressureResistance && pv[ny/CELL][nx/CELL] <= pressureResistance)
				{
					//@ PHOT + INVIS -> NEUT + INVIS
					part_change_type(i,x,y,PT_NEUT);
					parts[i].ctype = 0;
				}
				break;
			}
			case PT_BIZR:
			case PT_BIZRG:
			case PT_BIZRS:
				//@ PHOT + BIZR/BIZRG/BIZRS -> ELEC + BIZR/BIZRG/BIZRS
				part_change_type(i, x, y, PT_ELEC);
				parts[i].ctype = 0;
				break;
			case PT_H2:
				if (!(parts[i].tmp&0x1))
				{
					//@ PHOT + H2 -> PROT + ELEC
					part_change_type(i, x, y, PT_PROT);
					parts[i].ctype = 0;
					parts[i].tmp2 = 0x1;

					create_part(ID(r), x, y, PT_ELEC);
					return 1;
				}
				break;
			case PT_GPMP:
				if (parts[ID(r)].life == 0)
				{
					//@ PHOT + GPMP -> GRVT + GPMP
					part_change_type(i, x, y, PT_GRVT);
					parts[i].tmp = int(parts[ID(r)].temp - 273.15f);
				}
				break;
			}
			break;
		}
		case PT_NEUT:
			//@ NEUT + GLAS/BGLA -> NEUT + GLAS/BGLA + PHOT
			if (TYP(r) == PT_GLAS || TYP(r) == PT_BGLA)
				if (rng.chance(1, 10))
					create_cherenkov_photon(i);
			break;
		case PT_ELEC:
			if (TYP(r) == PT_GLOW)
			{
				//@ ELEC + GLOW -> PHOT + GLOW
				part_change_type(i, x, y, PT_PHOT);
				parts[i].ctype = 0x3FFFFFFF;
			}
			break;
		case PT_PROT:
			//@ PROT + INVIS -> NEUT + INVIS
			if (TYP(r) == PT_INVIS)
				part_change_type(i, x, y, PT_NEUT);
			break;
		case PT_BIZR:
		case PT_BIZRG:
			if (TYP(r) == PT_FILT)
				parts[i].ctype = Element_FILT_interactWavelengths(this, &parts[ID(r)], parts[i].ctype);
			break;
		}
		return 1;
	}
	//else e=1 , we are trying to swap the particles, return 0 no swap/move, 1 is still overlap/move, because the swap takes place later

	switch (TYP(r))
	{
	case PT_VOID:
	case PT_PVOD:
		// this is where void eats particles
		// void ctype already checked in eval_move
		kill_part(i);
		return 0;
	case PT_BHOL:
	case PT_NBHL:
		// this is where blackhole eats particles
		if (!legacy_enable)
		{
			parts[ID(r)].temp = restrict_flt(parts[ID(r)].temp+parts[i].temp/2, MIN_TEMP, MAX_TEMP);//3.0f;
		}
		kill_part(i);
		return 0;
	case PT_WHOL:
	case PT_NWHL:
		// whitehole eats anar
		if (parts[i].type == PT_ANAR)
		{
			if (!legacy_enable)
			{
				parts[ID(r)].temp = restrict_flt(parts[ID(r)].temp - (MAX_TEMP-parts[i].temp)/2, MIN_TEMP, MAX_TEMP);
			}
			kill_part(i);
			return 0;
		}
		break;
	case PT_DEUT:
		if (parts[i].type == PT_ELEC)
		{
			if(parts[ID(r)].life < 6000)
				parts[ID(r)].life += 1;
			parts[ID(r)].temp = 0;
			kill_part(i);
			return 0;
		}
		break;
	case PT_VIBR:
	case PT_BVBR:
		if ((elements[parts[i].type].Properties & TYPE_ENERGY))
		{
			parts[ID(r)].tmp += 20;
			kill_part(i);
			return 0;
		}
		break;
	}

	switch (parts[i].type)
	{
	case PT_NEUT:
		if (elements[TYP(r)].Properties & PROP_NEUTABSORB)
		{
			kill_part(i);
			return 0;
		}
		break;
	case PT_CNCT:
		{
			float cnctGravX, cnctGravY; // Calculate offset from gravity
			GetGravityField(x, y, elements[PT_CNCT].Gravity, elements[PT_CNCT].Gravity, cnctGravX, cnctGravY);
			int offsetX = 0, offsetY = 0;
			if (cnctGravX > 0.0f) offsetX++;
			else if (cnctGravX < 0.0f) offsetX--;
			if (cnctGravY > 0.0f) offsetY++;
			else if (cnctGravY < 0.0f) offsetY--;
			if ((offsetX != 0) != (offsetY != 0) && // Is this a different position (avoid diagonals, doesn't work well)
				((nx - x) * offsetX > 0 || (ny - y) * offsetY > 0) && // Is the destination particle below the moving particle
				(TYP(pmap[y+offsetY][x+offsetX]) == PT_CNCT || TYP(pmap[y+offsetY][x+offsetX]) == PT_ROCK)) //check below CNCT for another CNCT or ROCK
				return 0;
		}
		break;
	case PT_GBMB:
		if (parts[i].life > 0)
			return 0;
		break;
	}

	if ((bmap[y/CELL][x/CELL]==WL_EHOLE && !emap[y/CELL][x/CELL]) && !(bmap[ny/CELL][nx/CELL]==WL_EHOLE && !emap[ny/CELL][nx/CELL]))
		return 0;

	int ri = ID(r); //ri is the particle number at r (pmap[ny][nx])
	if (r)//the swap part, if we make it this far, swap
	{
		if (parts[i].type==PT_NEUT) {
			// target material is NEUTPENETRATE, meaning it gets moved around when neutron passes
			unsigned s = pmap[y][x];
			if (s && !(elements[TYP(s)].Properties&PROP_NEUTPENETRATE))
				return 1; // if the element currently underneath neutron isn't NEUTPENETRATE, don't move anything except the neutron
			// if nothing is currently underneath neutron, only move target particle
			if(bmap[y/CELL][x/CELL] == WL_ALLOWENERGY)
				return 1; // do not drag target particle into an energy only wall
			if (s)
			{
				pmap[ny][nx] = (s&~PMAPMASK)|parts[ID(s)].type;
				parts[ID(s)].x = float(nx);
				parts[ID(s)].y = float(ny);
			}
			else
				pmap[ny][nx] = 0;
			parts[ri].x = float(x);
			parts[ri].y = float(y);
			pmap[y][x] = PMAP(ri, parts[ri].type);
			return 1;
		}

		if (pmap[ny][nx] && ID(pmap[ny][nx]) == ri)
			pmap[ny][nx] = 0;
		if (swapProbeEnabled)
		{
			// * Distancia que ESTA troca impoe a particula deslocada. Chebyshev para casar
			//   com a metrica do halo, que e um quadrado em volta da faixa.
			auto swapDist = std::max(std::fabs(parts[i].x - parts[ri].x), std::fabs(parts[i].y - parts[ri].y));
			swapCumCount[ri] += 1;
			swapCumDist[ri] += swapDist;
		}
		parts[ri].x = parts[i].x;
		parts[ri].y = parts[i].y;
		int rx = int(parts[ri].x + 0.5f);
		int ry = int(parts[ri].y + 0.5f);
		pmap[ry][rx] = PMAP(ri, parts[ri].type);
	}
	return 1;
}

// try to move particle, and if successful update pmap and parts[i].x,y
int Simulation::do_move(int i, int x, int y, float nxf, float nyf)
{
	if (particleCostCollecting)
	{
		particleCostWorking.doMoveCalls += 1;
	}
	int nx = (int)(nxf+0.5f), ny = (int)(nyf+0.5f), result;
	if (edgeMode == EDGE_LOOP)
	{
		bool x_ok = (nx >= CELL && nx < XRES-CELL);
		bool y_ok = (ny >= CELL && ny < YRES-CELL);
		if (!x_ok)
			nxf = remainder_p(nxf-CELL+.5f, XRES-CELL*2.0f)+CELL-.5f;
		if (!y_ok)
			nyf = remainder_p(nyf-CELL+.5f, YRES-CELL*2.0f)+CELL-.5f;
		nx = (int)(nxf+0.5f);
		ny = (int)(nyf+0.5f);

		/*if (!x_ok || !y_ok)
		{
			//make sure there isn't something blocking it on the other side
			//only needed if this if statement is moved after the try_move (like my mod)
			//if (!eval_move(t, nx, ny, NULL) || (t == PT_PHOT && pmap[ny][nx]))
			//	return -1;
		}*/
	}
	if (parts[i].type == PT_NONE)
		return 0;
	result = try_move(i, x, y, nx, ny);
	if (result)
	{
		if (!move(i, x, y, nxf, nyf))
			return -1;
	}
	return result;
}

bool Simulation::move(int i, int x, int y, float nxf, float nyf)
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	int nx = (int)(nxf+0.5f), ny = (int)(nyf+0.5f);
	int t = parts[i].type;
	parts[i].x = nxf;
	parts[i].y = nyf;
	if (ny != y || nx != x)
	{
		if (pmap[y][x] && ID(pmap[y][x]) == i)
			pmap[y][x] = 0;
		if (photons[y][x] && ID(photons[y][x]) == i)
			photons[y][x] = 0;
		// kill_part if particle is out of bounds
		if (nx < CELL || nx >= XRES - CELL || ny < CELL || ny >= YRES - CELL)
		{
			kill_part(i);
			return false;
		}
		if (elements[t].Properties & TYPE_ENERGY)
			photons[ny][nx] = PMAP(i, t);
		else if (t)
			pmap[ny][nx] = PMAP(i, t);
	}

	return true;
}

void Simulation::photoelectric_effect(int nx, int ny)//create sparks from PHOT when hitting PSCN and NSCN
{
	unsigned r = pmap[ny][nx];

	if (TYP(r) == PT_PSCN && !parts[ID(r)].life)
	{
		if (TYP(pmap[ny][nx-1]) == PT_NSCN || TYP(pmap[ny][nx+1]) == PT_NSCN ||
		        TYP(pmap[ny-1][nx]) == PT_NSCN ||  TYP(pmap[ny+1][nx]) == PT_NSCN)
		{
			parts[ID(r)].ctype = PT_PSCN;
			part_change_type(ID(r), nx, ny, PT_SPRK);
			parts[ID(r)].life = 4;
		}
	}
}

unsigned static direction_to_map(float dx, float dy, int t)
{
	// TODO:
	// Adding extra directions causes some inaccuracies.
	// Not adding them causes problems with some diagonal surfaces (photons absorbed instead of reflected).
	// For now, don't add them.
	// Solution may involve more intelligent setting of initial i0 value in find_next_boundary?
	// or rewriting normal/boundary finding code

	return (dx >= 0) |
		   (((dx + dy) >= 0) << 1) |     /*  567  */
		   ((dy >= 0) << 2) |            /*  4+0  */
		   (((dy - dx) >= 0) << 3) |     /*  321  */
		   ((dx <= 0) << 4) |
		   (((dx + dy) <= 0) << 5) |
		   ((dy <= 0) << 6) |
		   (((dy - dx) <= 0) << 7);
	/*
	return (dx >= -0.001) |
		   (((dx + dy) >= -0.001) << 1) |     //  567
		   ((dy >= -0.001) << 2) |            //  4+0
		   (((dy - dx) >= -0.001) << 3) |     //  321
		   ((dx <= 0.001) << 4) |
		   (((dx + dy) <= 0.001) << 5) |
		   ((dy <= 0.001) << 6) |
		   (((dy - dx) <= 0.001) << 7);
	}*/
}

int Simulation::is_blocking(int t, int x, int y) const
{
	if (t & REFRACT) {
		if (x<0 || y<0 || x>=XRES || y>=YRES)
			return 0;
		if (TYP(pmap[y][x]) == PT_GLAS || TYP(pmap[y][x]) == PT_BGLA)
			return 1;
		return 0;
	}

	return !eval_move(t, x, y, nullptr);
}

int Simulation::is_boundary(int pt, int x, int y) const
{
	if (!is_blocking(pt,x,y))
		return 0;
	if (is_blocking(pt,x,y-1) && is_blocking(pt,x,y+1) && is_blocking(pt,x-1,y) && is_blocking(pt,x+1,y))
		return 0;
	return 1;
}

int Simulation::find_next_boundary(int pt, int *x, int *y, int dm, int *em, bool reverse) const
{
	static int dx[8] = {1,1,0,-1,-1,-1,0,1};
	static int dy[8] = {0,1,1,1,0,-1,-1,-1};
	static int de[8] = {0x83,0x07,0x0E,0x1C,0x38,0x70,0xE0,0xC1};

	if (*x <= 0 || *x >= XRES-1 || *y <= 0 || *y >= YRES-1)
	{
		return 0;
	}

	if (*em != -1)
	{
		dm &= de[*em];
	}

	unsigned int mask = 0;
	for (int i = 0; i < 8; ++i)
	{
		if ((dm & (1U << i)) && is_blocking(pt, *x + dx[i], *y + dy[i]))
		{
			mask |= (1U << i);
		}
	}
	for (int i = 0; i < 8; ++i)
	{
		int n = (i + (reverse ? 1 : -1)) & 7;
		if (((mask & (1U << i))) && !(mask & (1U << n)))
		{
			*x += dx[i];
			*y += dy[i];
			*em = i;
			return 1;
		}
	}

	return 0;
}

Simulation::GetNormalResult Simulation::get_normal(int pt, int x, int y, float dx, float dy) const
{
	int ldm, rdm, lm, rm;
	int lx, ly, lv, rx, ry, rv;
	int i, j;
	float r, ex, ey;

	if (!dx && !dy)
		return { false };

	if (!is_boundary(pt, x, y))
		return { false };

	ldm = (pt & REFRACT) ? 0xFF : direction_to_map(-dy, dx, pt);
	rdm = (pt & REFRACT) ? 0xFF : direction_to_map(dy, -dx, pt);
	lx = rx = x;
	ly = ry = y;
	lv = rv = 1;
	lm = rm = -1;

	j = 0;
	for (i=0; i<SURF_RANGE; i++) {
		if (lv)
			lv = find_next_boundary(pt, &lx, &ly, ldm, &lm, true);
		if (rv)
			rv = find_next_boundary(pt, &rx, &ry, rdm, &rm, false);
		j += lv + rv;
		if (!lv && !rv)
			break;
	}

	if (j < NORMAL_MIN_EST)
		return { false };

	if ((lx == rx) && (ly == ry))
		return { false };
	ex = float(rx - lx);
	ey = float(ry - ly);
	r = 1.0f/hypot(ex, ey);
	auto nx =  ey * r;
	auto ny = -ex * r;

	return { true, nx, ny, lx, ly, rx, ry };
}

template<bool PhotoelectricEffect, class Sim>
Simulation::GetNormalResult Simulation::get_normal_interp(Sim &sim, int pt, float x0, float y0, float dx, float dy)
{
	int x, y, i;

	dx /= NORMAL_FRAC;
	dy /= NORMAL_FRAC;

	for (i=0; i<NORMAL_INTERP; i++) {
		x = (int)(x0 + 0.5f);
		y = (int)(y0 + 0.5f);
		if (x < 0 || y < 0 || x >= XRES || y >= YRES)
		{
			return { false };
		}
		if (sim.is_boundary(pt, x, y))
			break;
		x0 += dx;
		y0 += dy;
	}
	if (i >= NORMAL_INTERP)
		return { false };

	if constexpr (PhotoelectricEffect)
	{
		if (pt == PT_PHOT)
			sim.photoelectric_effect(x, y);
	}

	return sim.get_normal(pt, x, y, dx, dy);
}

template
Simulation::GetNormalResult Simulation::get_normal_interp<false, const Simulation>(const Simulation &sim, int pt, float x0, float y0, float dx, float dy);

void Simulation::kill_part(int i)//kills particle number i
{
	if (i < 0 || i >= NPART)
		return;
	
	int x = (int)(parts[i].x + 0.5f);
	int y = (int)(parts[i].y + 0.5f);

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	int t = parts[i].type;
	if (t && elements[t].ChangeType)
	{
		(*(elements[t].ChangeType))(this, i, x, y, t, PT_NONE);
	}

	if (x >= 0 && y >= 0 && x < XRES && y < YRES)
	{
		if (pmap[y][x] && ID(pmap[y][x]) == i)
			pmap[y][x] = 0;
		else if (photons[y][x] && ID(photons[y][x]) == i)
			photons[y][x] = 0;
	}

	// This shouldn't happen but ... you never know?
	if (t == PT_NONE)
		return;

	elementCount[t]--;

	parts.Free(i);
	NUM_PARTS -= 1;
}

void Parts::Free(int i)
{
	data[i].type = PT_NONE;
	// * Devolve ao segmento de quem chamou, nao a um segmento derivado do indice. Isso e o
	//   ponto da segmentacao: uma thread nunca escreve na lista de outra, mesmo matando uma
	//   particula que outra thread tenha criado.
	data[i].life = pfree[currentSegment];
	pfree[currentSegment] = i;
}

// Changes the type of particle number i, to t.  This also changes pmap at the same time
// Returns true if the particle was killed
bool Simulation::part_change_type(int i, int x, int y, int t)
{
	if (x<0 || y<0 || x>=XRES || y>=YRES || i>=NPART || t<0 || t>=PT_NUM || !parts[i].type)
		return false;

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	if (!elements[t].Enabled || t == PT_NONE)
	{
		kill_part(i);
		return true;
	}
	if (elements[t].CreateAllowed)
	{
		if (!(*(elements[t].CreateAllowed))(this, i, x, y, t))
			return false;
	}

	if (elements[parts[i].type].ChangeType)
		(*(elements[parts[i].type].ChangeType))(this, i, x, y, parts[i].type, t);
	if (elements[t].ChangeType)
		(*(elements[t].ChangeType))(this, i, x, y, parts[i].type, t);

	if (parts[i].type > 0 && parts[i].type < PT_NUM && elementCount[parts[i].type])
		elementCount[parts[i].type]--;
	elementCount[t]++;

	parts[i].type = t;
	if (elements[t].Properties & TYPE_ENERGY)
	{
		photons[y][x] = PMAP(i, t);
		if (pmap[y][x] && ID(pmap[y][x]) == i)
			pmap[y][x] = 0;
	}
	else
	{
		pmap[y][x] = PMAP(i, t);
		if (photons[y][x] && ID(photons[y][x]) == i)
			photons[y][x] = 0;
	}
	return false;
}

//the function for creating a particle, use p=-1 for creating a new particle, -2 is from a brush, or a particle number to replace a particle.
//tv = Type (PMAPBITS bits) + Var (32-PMAPBITS bits), var is usually 0
int Simulation::create_part(int p, int x, int y, int t, int v)
{
	int i, oldType = PT_NONE;

	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	if (x<0 || y<0 || x>=XRES || y>=YRES || t<=0 || t>=PT_NUM || !elements[t].Enabled)
		return -1;

	if (t == PT_SPRK && p != -3 && !(p == -2 && elements[TYP(pmap[y][x])].CtypeDraw))
	{
		int type = TYP(pmap[y][x]);
		int index = ID(pmap[y][x]);
		if(type == PT_WIRE)
		{
			parts[index].ctype = PT_DUST;
			return index;
		}
		if (!(type == PT_INST || (elements[type].Properties&PROP_CONDUCTS)) || parts[index].life!=0)
			return -1;
		if (p == -2 && type == PT_INST)
		{
			FloodINST(x, y);
			return index;
		}
		parts[index].type = PT_SPRK;
		parts[index].life = 4;
		parts[index].ctype = type;
		pmap[y][x] = (pmap[y][x]&~PMAPMASK) | PT_SPRK;
		if (parts[index].temp+10.0f < 673.0f && !legacy_enable && (type==PT_METL || type == PT_BMTL || type == PT_BRMT || type == PT_PSCN || type == PT_NSCN || type == PT_ETRD || type == PT_NBLE || type == PT_IRON))
			parts[index].temp = parts[index].temp+10.0f;
		return index;
	}

	if (p == -2)
	{
		if (pmap[y][x])
		{
			int drawOn = TYP(pmap[y][x]);
			if (elements[drawOn].CtypeDraw)
				elements[drawOn].CtypeDraw(this, ID(pmap[y][x]), t, v);
			return -1;
		}
		else if (IsWallBlocking(x, y, t))
			return -1;
		else if (photons[y][x] && (elements[t].Properties & TYPE_ENERGY))
			return -1;
	}

	if (elements[t].CreateAllowed)
	{
		if (!(*(elements[t].CreateAllowed))(this, p, x, y, t))
			return -1;
	}

	if (p == -1 || //creating from anything but brush
	    p == -2 || //creating from brush
	    p == -3) //skip pmap checks, e.g. for sing explosion
	{
		if (p == -1)
		{
			// If there is a particle, only allow creation if the new particle can occupy the same space as the existing particle
			// If there isn't a particle but there is a wall, check whether the new particle is allowed to be in it
			//   (not "!=2" for wall check because eval_move returns 1 for moving into empty space)
			// If there's no particle and no wall, assume creation is allowed
			if (pmap[y][x] ? (eval_move(t, x, y, nullptr) != 2) : (bmap[y/CELL][x/CELL] && eval_move(t, x, y, nullptr) == 0))
			{
				return -1;
			}
		}
		i = parts.Alloc();
		if (i == -1)
		{
			return -1;
		}
		NUM_PARTS += 1;
	}
	else
	{
		int oldX = (int)(parts[p].x + 0.5f);
		int oldY = (int)(parts[p].y + 0.5f);

		if (InBounds(oldX, oldY))
		{
			if (pmap[oldY][oldX] && ID(pmap[oldY][oldX]) == p)
				pmap[oldY][oldX] = 0;
			if (photons[oldY][oldX] && ID(photons[oldY][oldX]) == p)
				photons[oldY][oldX] = 0;
		}

		oldType = parts[p].type;

		if (elements[oldType].ChangeType)
			(*(elements[oldType].ChangeType))(this, p, oldX, oldY, oldType, t);
		if (oldType)
			elementCount[oldType]--;

		i = p;
	}

	parts[i] = elements[t].DefaultProperties;
	parts[i].type = t;
	parts[i].x = (float)x;
	parts[i].y = (float)y;

	//and finally set the pmap/photon maps to the newly created particle
	if (elements[t].Properties & TYPE_ENERGY)
		photons[y][x] = PMAP(i, t);
	else if (t!=PT_STKM && t!=PT_STKM2 && t!=PT_FIGH)
		pmap[y][x] = PMAP(i, t);

	//Fancy dust effects for powder types
	if((elements[t].Properties & TYPE_PART) && pretty_powder)
	{
		int colr, colg, colb;
		int sandcolourToUse = p == -2 ? sandcolour_interface : sandcolour;
		RGB colour = elements[t].Colour;
		colr = colour.Red   + int(sandcolourToUse * 1.3) + rng.between(-20, 20) + rng.between(-15, 15);
		colg = colour.Green + int(sandcolourToUse * 1.3) + rng.between(-20, 20) + rng.between(-15, 15);
		colb = colour.Blue  + int(sandcolourToUse * 1.3) + rng.between(-20, 20) + rng.between(-15, 15);
		colr = std::clamp(colr, 0, 255);
		colg = std::clamp(colg, 0, 255);
		colb = std::clamp(colb, 0, 255);
		parts[i].dcolour = (rng.between(0, 149)<<24) | (colr<<16) | (colg<<8) | colb;
	}

	// Set non-static properties (such as randomly generated ones)
	if (elements[t].Create)
		(*(elements[t].Create))(this, i, x, y, t, v);

	if (elements[t].ChangeType)
		(*(elements[t].ChangeType))(this, i, x, y, oldType, t);

	elementCount[t]++;
	return i;
}

// Change part type but preserve temperature and velocity.
// May fail if i isn't an existing particle id.
int Simulation::createPartTempVel(int i, int x, int y, int t)
{
	auto temp = parts[i].temp;
	auto vx = parts[i].vx;
	auto vy = parts[i].vy;

	auto np = create_part(i, x, y, t);
	if (np >= 0)
	{
		parts[np].temp = temp;
		parts[np].vx = vx;
		parts[np].vy = vy;
	}

	return np;
}

int Parts::Alloc()
{
	if (pfree[currentSegment] != -1)
	{
		auto i = pfree[currentSegment];
		pfree[currentSegment] = data[i].life;
		localAllocCount += 1;
		return i;
	}
	// * Segmento proprio vazio: varre os demais em ordem fixa antes de crescer `active`.
	//   Sem esse resgate, slots liberados por outra thread ficariam encalhados e a simulacao
	//   bateria no teto de particulas com memoria livre sobrando. A ordem fixa mantem o
	//   resultado deterministico.
	for (auto segment = 0; segment < freeSegments; segment++)
	{
		if (pfree[segment] != -1)
		{
			auto i = pfree[segment];
			pfree[segment] = data[i].life;
			rescueCount += 1;
			return i;
		}
	}
	if (active < NPART)
	{
		auto i = active;
		active += 1;
		return i;
	}
	return -1;
}

void Simulation::create_gain_photon(int pp)//photons from PHOT going through GLOW
{
	if (parts.MaxPartsReached())
	{
		return;
	}
	auto lr = 2 * rng.between(0, 1) - 1; // -1 or 1
	auto xx = parts[pp].x - lr * 0.3f * parts[pp].vy;
	auto yy = parts[pp].y + lr * 0.3f * parts[pp].vx;
	auto nx = int(xx + 0.5f);
	auto ny = int(yy + 0.5f);
	if (nx < 0 || ny < 0 || nx >= XRES || ny >= YRES)
	{
		return;
	}
	auto g = pmap[ny][nx];
	if (TYP(g) != PT_GLOW)
	{
		return;
	}
	auto oldTemp = parts[ID(g)].temp;
	auto i = create_part(-3, nx, ny, PT_PHOT);
	if (i == -1)
	{
		return;
	}
	parts[i].x = xx;
	parts[i].y = yy;
	parts[i].vx = parts[pp].vx;
	parts[i].vy = parts[pp].vy;
	parts[i].temp = oldTemp;
	auto temp_bin = int((parts[i].temp - 273.0f) * 0.25f);
	if (temp_bin < 0) temp_bin = 0;
	if (temp_bin > 25) temp_bin = 25;
	parts[i].ctype = 0x1F << temp_bin;
}

void Simulation::create_cherenkov_photon(int pp)//photons from NEUT going through GLAS
{
	if (parts.MaxPartsReached())
	{
		return;
	}
	auto nx = int(parts[pp].x + 0.5f);
	auto ny = int(parts[pp].y + 0.5f);
	auto g = pmap[ny][nx];
	if (TYP(g) != PT_GLAS && TYP(g) != PT_BGLA)
	{
		return;
	}
	if (std::hypot(parts[pp].vx, parts[pp].vy) < 1.44f)
	{
		return;
	}
	auto oldTemp = parts[ID(g)].temp;
	auto i = create_part(-3, nx, ny, PT_PHOT);
	if (i == -1)
	{
		return;
	}
	auto lr = 2 * rng.between(0, 1) - 1; // -1 or 1
	parts[i].ctype = 0x00000F80;
	parts[i].x = parts[pp].x;
	parts[i].y = parts[pp].y;
	parts[i].temp = oldTemp;
	parts[i].vx = parts[pp].vx - lr * 2.5f * parts[pp].vy;
	parts[i].vy = parts[pp].vy + lr * 2.5f * parts[pp].vx;
	/* photons have speed of light. no discussion. */
	auto r = 1.269f / std::hypot(parts[i].vx, parts[i].vy);
	parts[i].vx *= r;
	parts[i].vy *= r;
}

void Simulation::GetGravityField(int x, int y, float particleGrav, float newtonGrav, float & pGravX, float & pGravY) const
{
	switch (gravityMode)
	{
	default:
	case GRAV_VERTICAL: //normal, vertical gravity
		pGravX = 0;
		pGravY = particleGrav;
		break;
	case GRAV_OFF: //no gravity
		pGravX = 0;
		pGravY = 0;
		break;
	case GRAV_RADIAL: //radial gravity
		{
			pGravX = 0;
			pGravY = 0;
			auto dx = float(x - XCNTR);
			auto dy = float(y - YCNTR);
			if (dx || dy)
			{
				auto pGravD = 0.01f - hypotf(dx, dy);
				pGravX = particleGrav * (dx / pGravD);
				pGravY = particleGrav * (dy / pGravD);
			}
		}
		break;
	case GRAV_CUSTOM: //custom gravity
		pGravX = particleGrav * customGravityX;
		pGravY = particleGrav * customGravityY;
		break;
	}
	if (newtonGrav)
	{
		pGravX += newtonGrav * gravOut.forceX[Vec2{ x, y } / CELL];
		pGravY += newtonGrav * gravOut.forceY[Vec2{ x, y } / CELL];
	}
}

void Simulation::delete_part(int x, int y)//calls kill_part with the particle located at x,y
{
	unsigned i;

	if (x<0 || y<0 || x>=XRES || y>=YRES)
		return;

	i = photons[y][x] ? photons[y][x] : pmap[y][x];

	if (!i)
		return;
	kill_part(ID(i));
}

template<bool UpdateEmap, class Sim>
Simulation::PlanMoveResult Simulation::PlanMove(Sim &sim, int i, int x, int y)
{
	auto &parts = sim.parts;
	auto &bmap = sim.bmap;
	auto &emap = sim.emap;
	auto &pmap = sim.pmap;
	auto edgeMode = sim.edgeMode;
	auto &sd = SimulationData::CRef();
	auto &can_move = sd.can_move;
	auto t = parts[i].type;
	int fin_x, fin_y, clear_x, clear_y;
	float fin_xf, fin_yf, clear_xf, clear_yf;
	auto vx = parts[i].vx;
	auto vy = parts[i].vy;
	auto mv = fmaxf(fabsf(vx), fabsf(vy));
	if (mv < ISTP || std::isnan(mv))
	{
		clear_x = x;
		clear_y = y;
		clear_xf = parts[i].x;
		clear_yf = parts[i].y;
		fin_xf = clear_xf + vx;
		fin_yf = clear_yf + vy;
		fin_x = (int)(fin_xf+0.5f);
		fin_y = (int)(fin_yf+0.5f);
	}
	else
	{
		if (mv > MAX_VELOCITY)
		{
			vx *= MAX_VELOCITY/mv;
			vy *= MAX_VELOCITY/mv;
			mv = MAX_VELOCITY;
		}
		// interpolate to see if there is anything in the way
		auto dx = vx*ISTP/mv;
		auto dy = vy*ISTP/mv;
		fin_xf = parts[i].x;
		fin_yf = parts[i].y;
		fin_x = (int)(fin_xf+0.5f);
		fin_y = (int)(fin_yf+0.5f);
		bool closedEholeStart = InBounds(fin_x, fin_y) && (bmap[fin_y/CELL][fin_x/CELL] == WL_EHOLE && !emap[fin_y/CELL][fin_x/CELL]);
		while (1)
		{
			mv -= ISTP;
			fin_xf += dx;
			fin_yf += dy;
			fin_x = (int)(fin_xf+0.5f);
			fin_y = (int)(fin_yf+0.5f);
			if (edgeMode == EDGE_LOOP)
			{
				bool x_ok = (fin_xf >= CELL-.5f && fin_xf < XRES-CELL-.5f);
				bool y_ok = (fin_yf >= CELL-.5f && fin_yf < YRES-CELL-.5f);
				if (!x_ok)
					fin_xf = remainder_p(fin_xf-CELL+.5f, XRES-CELL*2.0f)+CELL-.5f;
				if (!y_ok)
					fin_yf = remainder_p(fin_yf-CELL+.5f, YRES-CELL*2.0f)+CELL-.5f;
				fin_x = (int)(fin_xf+0.5f);
				fin_y = (int)(fin_yf+0.5f);
			}
			if (mv <= 0.0f)
			{
				// nothing found
				fin_xf = parts[i].x + vx;
				fin_yf = parts[i].y + vy;
				if (edgeMode == EDGE_LOOP)
				{
					bool x_ok = (fin_xf >= CELL-.5f && fin_xf < XRES-CELL-.5f);
					bool y_ok = (fin_yf >= CELL-.5f && fin_yf < YRES-CELL-.5f);
					if (!x_ok)
						fin_xf = remainder_p(fin_xf-CELL+.5f, XRES-CELL*2.0f)+CELL-.5f;
					if (!y_ok)
						fin_yf = remainder_p(fin_yf-CELL+.5f, YRES-CELL*2.0f)+CELL-.5f;
				}
				fin_x = (int)(fin_xf+0.5f);
				fin_y = (int)(fin_yf+0.5f);
				clear_xf = fin_xf-dx;
				clear_yf = fin_yf-dy;
				clear_x = (int)(clear_xf+0.5f);
				clear_y = (int)(clear_yf+0.5f);
				break;
			}
			//block if particle can't move (0), or some special cases where it returns 1 (can_move = 3 but returns 1 meaning particle will be eaten)
			//also photons are still blocked (slowed down) by any particle (even ones it can move through), and absorb wall also blocks particles
			int eval = sim.eval_move(t, fin_x, fin_y, nullptr);
			if (!eval || (can_move[t][TYP(pmap[fin_y][fin_x])] == 3 && eval == 1) || (t == PT_PHOT && pmap[fin_y][fin_x]) || bmap[fin_y/CELL][fin_x/CELL]==WL_DESTROYALL || closedEholeStart!=(bmap[fin_y/CELL][fin_x/CELL] == WL_EHOLE && !emap[fin_y/CELL][fin_x/CELL]))
			{
				// found an obstacle
				clear_xf = fin_xf-dx;
				clear_yf = fin_yf-dy;
				clear_x = (int)(clear_xf+0.5f);
				clear_y = (int)(clear_yf+0.5f);
				break;
			}
			if constexpr (UpdateEmap)
			{
				if (sim.bmap[fin_y/CELL][fin_x/CELL]==WL_DETECT && sim.emap[fin_y/CELL][fin_x/CELL]<8)
					sim.set_emap(fin_x/CELL, fin_y/CELL);
			}
		}
	}
	return {
		fin_x,
		fin_y,
		clear_x,
		clear_y,
		fin_xf,
		fin_yf,
		clear_xf,
		clear_yf,
		vx,
		vy,
	};
}

template
Simulation::PlanMoveResult Simulation::PlanMove<false, const Simulation>(const Simulation &sim, int i, int x, int y);

std::unique_ptr<Simulation> Simulation::Factory()
{
	return std::make_unique<SimulationImpl>();
}

SimulationImpl::Neighbourhood SimulationImpl::GetNeighbourhood(int i) const
{
	auto t = parts[i].type;
	auto x = int(parts[i].x + 0.5f);
	auto y = int(parts[i].y + 0.5f);
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	Neighbourhood n;
	auto j = 0;
	for (auto nx=-1; nx<2; nx++)
	{
		for (auto ny=-1; ny<2; ny++)
		{
			if (nx||ny)
			{
				auto r = pmap[y+ny][x+nx];
				n.surround[j] = r;
				j++;
				n.surround_space += (!TYP(r)); // count empty space
				n.nt += (TYP(r)!=t); // count empty space and particles of different type
			}
		}
	}
	if (!(elements[t].Properties & TYPE_SOLID) && (elements[t].Gravity || elements[t].NewtonianGravity))
	{
		GetGravityField(x, y, elements[t].Gravity, elements[t].NewtonianGravity, n.pGravX, n.pGravY);
	}
	return n;
}

namespace
{
	// * Reach probe. Splitting the particle loop across threads by region is only sound if a
	//   single particle's update touches a bounded neighbourhood. This measures the real
	//   per-frame displacement (the write side of that reach) instead of assuming a bound:
	//   snapshot every live particle's position on entry, compare on exit, and bucket the
	//   Chebyshev distance, which is the relevant metric because a square halo around a
	//   region is what a decomposition would have to reserve. Inactive unless TPT_REACH_CSV
	//   is set, so normal play pays nothing.
	struct ReachProbe
	{
		static std::FILE *file;
		static bool checked;
		static int frameCounter;

		const Particle *parts;
		std::vector<float> beforeX, beforeY;
		std::vector<int> beforeType;

		explicit ReachProbe(const Particle *newParts) : parts(newParts)
		{
			if (!checked)
			{
				checked = true;
				if (auto *path = std::getenv("TPT_REACH_CSV"))
				{
					file = std::fopen(path, "w");
					if (file)
					{
						std::fprintf(file, "frame,moved,max_cheb,max_elem,gt1,gt2,gt4,gt8,gt16,gt32,gt64\n");
					}
				}
			}
			if (!file)
			{
				return;
			}
			beforeX.resize(NPART);
			beforeY.resize(NPART);
			beforeType.resize(NPART);
			for (auto i = 0; i < NPART; i++)
			{
				beforeType[i] = parts[i].type;
				beforeX[i] = parts[i].x;
				beforeY[i] = parts[i].y;
			}
		}

		~ReachProbe()
		{
			if (!file)
			{
				return;
			}
			frameCounter += 1;
			int moved = 0;
			float maxCheb = 0.f;
			// * Which element produced the longest jump: a decomposition can only exclude the
			//   long-reach cases from the parallel path if it knows which ones they are.
			int maxType = 0;
			int buckets[7] = {};
			constexpr float thresholds[7] = { 1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f };
			for (auto i = 0; i < NPART; i++)
			{
				// * Only particles that survived the frame as the same type are comparable;
				//   a recycled slot would report the distance between two unrelated particles.
				if (!beforeType[i] || parts[i].type != beforeType[i])
				{
					continue;
				}
				auto dx = std::fabs(parts[i].x - beforeX[i]);
				auto dy = std::fabs(parts[i].y - beforeY[i]);
				auto cheb = std::max(dx, dy);
				if (cheb > 0.f)
				{
					moved += 1;
				}
				if (cheb > maxCheb)
				{
					maxCheb = cheb;
					maxType = beforeType[i];
				}
				for (auto b = 0; b < 7; b++)
				{
					if (cheb > thresholds[b])
					{
						buckets[b] += 1;
					}
				}
			}
			auto &sd = SimulationData::CRef();
			auto maxName = (maxType > 0 && maxType < PT_NUM) ? sd.elements[maxType].Name.ToUtf8() : ByteString("none");
			std::fprintf(file, "%d,%d,%.2f,%s,%d,%d,%d,%d,%d,%d,%d\n", frameCounter, moved, maxCheb, maxName.c_str(),
				buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5], buckets[6]);
			std::fflush(file);
		}
	};
	std::FILE *ReachProbe::file = nullptr;
	bool ReachProbe::checked = false;
	int ReachProbe::frameCounter = 0;

	// ---- Classificacao para decomposicao espacial (estagio 3) -------------------------
	// * Halo que uma faixa teria de reservar. 32 px cobre a busca lateral de liquidos
	//   (rt = 30, medido) e as varreduras de vizinhanca dos elementos (raio 1 ou 2, com
	//   STKM em 4 e DTEC limitado a 25 pelo proprio codigo).
	constexpr float DecompHalo = 32.0f;

	// * Elementos cuja LEITURA nao tem limite espacial: detectores que varrem em linha,
	//   emissores de raio, canais de WIFI, portais e o gatilho global do EMP. Ao contrario
	//   das escritas longas, que vem de materia comum advectada e portanto nao sao
	//   excluiveis por tipo, estes sao enumeraveis e vao para um passe serial.
	bool IsUnboundedReader(int type)
	{
		switch (type)
		{
		case PT_LDTC: case PT_ETRD: case PT_ARAY: case PT_CRAY: case PT_DRAY:
		case PT_WIFI: case PT_PRTI: case PT_PRTO: case PT_EMP:
		case PT_STKM: case PT_STKM2: case PT_FIGH:
			return true;
		default:
			return false;
		}
	}

	uint64_t ParticleCostNs(ParticleCostClock::duration duration)
	{
		return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
	}

	ParticleCostClass ClassifyParticleCost(int type, const std::array<Element, PT_NUM> &elements)
	{
		if (!type)
		{
			return ParticleCostClass::LoopDead;
		}
		auto state = elements[type].Properties & STATE_FLAGS;
		if (IsUnboundedReader(type))
		{
			return state == TYPE_SOLID ? ParticleCostClass::SolidSpecial : ParticleCostClass::ActorSpecial;
		}
		switch (state)
		{
		case TYPE_PART:   return ParticleCostClass::Powder;
		case TYPE_SOLID:  return ParticleCostClass::SolidLocal;
		case TYPE_LIQUID: return ParticleCostClass::Liquid;
		case TYPE_GAS:    return ParticleCostClass::Gas;
		case TYPE_ENERGY: return ParticleCostClass::Energy;
		default:          return ParticleCostClass::StateOther;
		}
	}

	bool IsParticleCostCandidate(ParticleCostClass value)
	{
		return value == ParticleCostClass::Powder || value == ParticleCostClass::SolidLocal;
	}

	bool IsParticleCostFluid(ParticleCostClass value)
	{
		return value == ParticleCostClass::Liquid || value == ParticleCostClass::Gas;
	}

	void CalibrateParticleCostClock()
	{
		std::vector<uint64_t> samples;
		samples.reserve(ParticleCostClockCalibrationBatches * ParticleCostClockCalibrationSamplesPerBatch);
		std::array<double, ParticleCostClockCalibrationBatches> batchMeans = {};
		for (auto batch = 0; batch < ParticleCostClockCalibrationBatches; ++batch)
		{
			long double batchTotal = 0.0L;
			for (auto i = 0; i < ParticleCostClockCalibrationSamplesPerBatch; ++i)
			{
				auto begin = ParticleCostClock::now();
				auto end = ParticleCostClock::now();
				auto elapsed = ParticleCostNs(end - begin);
				samples.push_back(elapsed);
				batchTotal += elapsed;
			}
			batchMeans[batch] = double(batchTotal / ParticleCostClockCalibrationSamplesPerBatch);
		}
		std::sort(samples.begin(), samples.end());
		std::sort(batchMeans.begin(), batchMeans.end());
		particleCostClockMinNs = samples.front();
		particleCostClockP50Ns = samples[samples.size() / 2];
		particleCostClockBatchMeanMedianNs =
			(batchMeans[ParticleCostClockCalibrationBatches / 2 - 1] +
			 batchMeans[ParticleCostClockCalibrationBatches / 2]) / 2.0;
	}

	void EnsureParticleCostFile()
	{
		if (particleCostChecked)
		{
			return;
		}
		particleCostChecked = true;
		if (auto *stride = std::getenv("TPT_UPDATE_COST_SAMPLE_STRIDE"))
		{
			particleCostSampleStride = std::clamp(std::atoi(stride), 1, NPART);
		}
		if (auto *path = std::getenv("TPT_UPDATE_COST_CSV"))
		{
			particleCostFile = std::fopen(path, "w");
		}
		if (!particleCostFile)
		{
			return;
		}
		CalibrateParticleCostClock();
		std::fprintf(particleCostFile,
			"frame,update_calls,update_ns,loop_ns,class_sum_ns,fixed_ns,class_coverage_error_ns,"
			"update_remainder_raw_ns,move_est_raw_ns,update_remainder_min_corrected_ns,move_est_min_corrected_ns,"
			"update_remainder_p50_corrected_ns,move_est_p50_corrected_ns,"
			"update_remainder_calibrated_ns,move_est_calibrated_ns,"
			"sample_stride,clock_min_ns,clock_p50_ns,clock_batch_mean_median_ns,"
			"clock_calibration_batches,clock_calibration_samples_per_batch,"
			"slots_seen,live_seen,dead_slots,class_switches,movement_calls,sampled_movement_calls,try_move_calls,"
			"do_move_calls,lateral_vertical_entries,lateral_gravity_entries,lateral_search_steps,element_update_calls,"
			"entry_movement_class_changes,candidate_to_fluid_changes");
		for (auto *name : particleCostClassNames)
		{
			std::fprintf(particleCostFile, ",%s_count,%s_ns,%s_move_calls,%s_move_samples,%s_move_sample_raw_ns",
				name, name, name, name, name);
		}
		std::fprintf(particleCostFile, "\n");
	}

	uint64_t EstimateParticleMovementNs(const ParticleCostFrame &frame, double intervalOverheadNs)
	{
		uint64_t samples = 0;
		uint64_t rawNs = 0;
		for (size_t i = 0; i < ParticleCostClassCount; ++i)
		{
			samples += frame.sampledMovementCalls[i];
			rawNs += frame.sampledMovementRawNs[i];
		}
		auto overhead = static_cast<long double>(intervalOverheadNs) * samples;
		auto adjusted = std::max(0.0L, static_cast<long double>(rawNs) - overhead);
		// The rotating modulo sample gives every particle index probability 1 / stride
		// over a complete cycle. Expanding each observation by stride keeps rare classes
		// represented when a particular frame has calls but no sample in that class.
		auto estimate = adjusted * particleCostSampleStride;
		return uint64_t(estimate + 0.5L);
	}

	class ParticleCostProbe
	{
		Simulation *owner = nullptr;
		bool enabled = false;
		bool loopActive = false;
		ParticleCostClass currentClass = ParticleCostClass::LoopDead;
		ParticleCostClass entryClass = ParticleCostClass::LoopDead;
		ParticleCostClock::time_point updateStartedAt;
		ParticleCostClock::time_point loopStartedAt;
		ParticleCostClock::time_point classStartedAt;

	public:
		explicit ParticleCostProbe(Simulation *newOwner):
			owner(newOwner)
		{
			EnsureParticleCostFile();
			enabled = particleCostFile != nullptr;
			if (enabled)
			{
				AcquireParticleCostOwner(owner);
				particleCostWorking = {};
				particleCostWorking.updateCalls = 1;
				updateStartedAt = ParticleCostClock::now();
			}
		}

		~ParticleCostProbe()
		{
			if (!enabled || particleCostOwner != owner)
			{
				return;
			}
			if (loopActive)
			{
				EndLoop();
			}
			particleCostWorking.updateNs += ParticleCostNs(ParticleCostClock::now() - updateStartedAt);
			particleCostPending.Add(particleCostWorking);
			particleCostPendingReady = true;
		}

		void BeginLoop()
		{
			if (!enabled)
			{
				return;
			}
			loopActive = true;
			currentClass = ParticleCostClass::LoopDead;
			entryClass = currentClass;
			loopStartedAt = classStartedAt = ParticleCostClock::now();
			particleCostCollecting = true;
		}

		void EnterSlot(int type, const std::array<Element, PT_NUM> &elements)
		{
			if (!enabled)
			{
				return;
			}
			particleCostWorking.slotsSeen += 1;
			entryClass = ClassifyParticleCost(type, elements);
			auto index = size_t(entryClass);
			particleCostWorking.classCount[index] += 1;
			if (type)
			{
				particleCostWorking.liveSeen += 1;
			}
			else
			{
				particleCostWorking.deadSlots += 1;
			}
			if (entryClass != currentClass)
			{
				auto now = ParticleCostClock::now();
				particleCostWorking.classNs[size_t(currentClass)] += ParticleCostNs(now - classStartedAt);
				classStartedAt = now;
				currentClass = entryClass;
				particleCostWorking.classSwitches += 1;
			}
		}

		void EndLoop()
		{
			if (!enabled || !loopActive)
			{
				return;
			}
			auto now = ParticleCostClock::now();
			particleCostWorking.classNs[size_t(currentClass)] += ParticleCostNs(now - classStartedAt);
			particleCostWorking.loopNs += ParticleCostNs(now - loopStartedAt);
			particleCostCollecting = false;
			loopActive = false;
		}

		void RecordElementUpdate()
		{
			if (enabled)
			{
				particleCostWorking.elementUpdateCalls += 1;
			}
		}

		bool ShouldSampleMovement(int particleIndex, int movementType, const std::array<Element, PT_NUM> &elements)
		{
			if (!enabled)
			{
				return false;
			}
			auto entryIndex = size_t(entryClass);
			particleCostWorking.movementCalls[entryIndex] += 1;
			auto movementClass = ClassifyParticleCost(movementType, elements);
			if (movementClass != entryClass)
			{
				particleCostWorking.entryMovementClassChanges += 1;
				if (IsParticleCostCandidate(entryClass) && IsParticleCostFluid(movementClass))
				{
					particleCostWorking.candidateToFluidChanges += 1;
				}
			}
			auto sample = (uint64_t(particleIndex) + uint64_t(particleCostFrameCounter + 1)) %
				uint64_t(particleCostSampleStride) == 0;
			if (sample)
			{
				particleCostWorking.sampledMovementCalls[entryIndex] += 1;
			}
			return sample;
		}

		void RecordMovement(ParticleCostClock::duration duration)
		{
			if (enabled)
			{
				particleCostWorking.sampledMovementRawNs[size_t(entryClass)] +=
					ParticleCostNs(duration);
			}
		}
	};

	void DumpParticleCostFrame(const Simulation *owner)
	{
		if (!particleCostFile || particleCostOwner != owner || !particleCostPendingReady)
		{
			return;
		}
		auto &frame = particleCostPending;
		uint64_t classSumNs = 0;
		uint64_t movementCalls = 0;
		uint64_t sampledMovementCalls = 0;
		for (size_t i = 0; i < ParticleCostClassCount; ++i)
		{
			classSumNs += frame.classNs[i];
			movementCalls += frame.movementCalls[i];
			sampledMovementCalls += frame.sampledMovementCalls[i];
		}
		auto moveRawNs = EstimateParticleMovementNs(frame, 0.0);
		auto moveMinNs = EstimateParticleMovementNs(frame, static_cast<double>(particleCostClockMinNs));
		auto moveP50Ns = EstimateParticleMovementNs(frame, static_cast<double>(particleCostClockP50Ns));
		auto moveCalibratedNs = EstimateParticleMovementNs(frame, particleCostClockBatchMeanMedianNs);
		auto fixedNs = int64_t(frame.updateNs) - int64_t(classSumNs);
		auto classCoverageErrorNs = int64_t(frame.loopNs) - int64_t(classSumNs);
		auto remainderRawNs = int64_t(frame.updateNs) - int64_t(moveRawNs);
		auto remainderMinNs = int64_t(frame.updateNs) - int64_t(moveMinNs);
		auto remainderP50Ns = int64_t(frame.updateNs) - int64_t(moveP50Ns);
		auto remainderCalibratedNs = int64_t(frame.updateNs) - int64_t(moveCalibratedNs);
		particleCostFrameCounter += 1;
		std::fprintf(particleCostFile,
			"%d,%llu,%llu,%llu,%llu,%lld,%lld,%lld,%llu,%lld,%llu,%lld,%llu,%lld,%llu,%d,%llu,%llu,%.6f,%d,%d,"
			"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
			particleCostFrameCounter,
			(unsigned long long)frame.updateCalls,
			(unsigned long long)frame.updateNs,
			(unsigned long long)frame.loopNs,
			(unsigned long long)classSumNs,
			(long long)fixedNs,
			(long long)classCoverageErrorNs,
			(long long)remainderRawNs,
			(unsigned long long)moveRawNs,
			(long long)remainderMinNs,
			(unsigned long long)moveMinNs,
			(long long)remainderP50Ns,
			(unsigned long long)moveP50Ns,
			(long long)remainderCalibratedNs,
			(unsigned long long)moveCalibratedNs,
			particleCostSampleStride,
			(unsigned long long)particleCostClockMinNs,
			(unsigned long long)particleCostClockP50Ns,
			particleCostClockBatchMeanMedianNs,
			ParticleCostClockCalibrationBatches,
			ParticleCostClockCalibrationSamplesPerBatch,
			(unsigned long long)frame.slotsSeen,
			(unsigned long long)frame.liveSeen,
			(unsigned long long)frame.deadSlots,
			(unsigned long long)frame.classSwitches,
			(unsigned long long)movementCalls,
			(unsigned long long)sampledMovementCalls,
			(unsigned long long)frame.tryMoveCalls,
			(unsigned long long)frame.doMoveCalls,
			(unsigned long long)frame.lateralVerticalEntries,
			(unsigned long long)frame.lateralGravityEntries,
			(unsigned long long)frame.lateralSearchSteps,
			(unsigned long long)frame.elementUpdateCalls,
			(unsigned long long)frame.entryMovementClassChanges,
			(unsigned long long)frame.candidateToFluidChanges);
		for (size_t i = 0; i < ParticleCostClassCount; ++i)
		{
			std::fprintf(particleCostFile, ",%llu,%llu,%llu,%llu,%llu",
				(unsigned long long)frame.classCount[i],
				(unsigned long long)frame.classNs[i],
				(unsigned long long)frame.movementCalls[i],
				(unsigned long long)frame.sampledMovementCalls[i],
				(unsigned long long)frame.sampledMovementRawNs[i]);
		}
		std::fprintf(particleCostFile, "\n");
		std::fflush(particleCostFile);
		particleCostPending = {};
		particleCostPendingReady = false;
	}

	// * Contadores do frame corrente. serialType = leitura ilimitada; serialMove = escrita
	//   longa prevista; mispredict = classificado como paralelo mas que ANDOU mais que o
	//   halo, ou seja, falha de seguranca do preditor. Esse ultimo e o numero que decide se
	//   a classificacao pode ser confiada.
	long long clsParallel = 0, clsSerialType = 0, clsSerialMove = 0, clsMispredict = 0;
	// * Posicao e tipo na classificacao anterior de cada slot, para medir o deslocamento
	//   efetivo de um frame e confrontar com o que o preditor disse.
	std::vector<float> clsPrevX, clsPrevY;
	std::vector<int> clsPrevType;
	std::vector<char> clsPrevParallel;

	// * Histograma acumulado de mispredicts por elemento, com o pior deslocamento visto.
	//   Sem isto a causa dos saltos longos fica sendo inferencia; com isto vira medicao:
	//   se forem liquidos e a maioria dos saltos cair em 30 px, a busca lateral (rt = 30)
	//   esta confirmada como mecanismo.
	std::vector<long long> clsMispredictByType;
	std::vector<float> clsMispredictMaxDist;
	std::vector<long long> clsMispredictAt30;   // saltos na assinatura exata da busca lateral

	// * Estado de troca no frame anterior, para isolar quanto do salto veio de troca.
	std::vector<long long> clsPrevSwapCount;
	std::vector<float> clsPrevSwapDist;
	// * Agregados so dos mispredicts: quantas trocas sofreram e quanto do deslocamento
	//   essas trocas explicam. Se as trocas responderem por quase todo o salto, o mecanismo
	//   esta confirmado; se responderem por pouco, a causa e outra e continua em aberto.
	long long mispredictSwapTotal = 0, mispredictNoSwap = 0, mispredictWithSwap = 0;
	long long mispredictMaxSwaps = 0;
	double mispredictDistSum = 0.0, mispredictSwapDistSum = 0.0;
}

void SimulationImpl::UpdateParticles(int start, int end)
{
	// * Declared before the FrameTime span so its destructor samples the end only after the
	//   outer span has closed. CSV I/O is deferred to AfterSim and never enters this timing.
	ParticleCostProbe costProbe(this);
	FrameTime::Span span(frameTime, "Simulation::UpdateParticles");
	ReachProbe reachProbe(parts.data.data());
	// * Dentro do laco o RNG e re-semeado por particula, o que descarta o estado corrente.
	//   Simulation::rng tambem e usado fora do laco (BeforeSim, CheckStacking, ferramentas),
	//   e esses usuarios continuam num fluxo sequencial proprio; salvar e restaurar mantem
	//   esse fluxo intacto em vez de deixa-lo com o resto da ultima particula processada.
	auto savedRngState = rng.state();
	Defer restoreRngState([this, savedRngState]() { rng.state(savedRngState); });
	// * Ativa a atribuicao de segmento por faixa (ver dentro do laco). Fora do laco o
	//   segmento volta a 0, porque criacao vinda de ferramentas e save/load nao pertence a
	//   faixa nenhuma.
	static const bool freeListBanding = []() {
		auto *env = std::getenv("TPT_FREELIST_BANDING");
		return env && std::atoi(env) != 0;
	}();
	// * Classificacao de decomposicao (estagio 3): so mede, nao muda ordem nem comportamento.
	static const bool classifyEnabled = []() {
		auto *env = std::getenv("TPT_CLASSIFY");
		return env && std::atoi(env) != 0;
	}();
	if (classifyEnabled && clsPrevType.empty())
	{
		clsPrevX.assign(NPART, 0.0f);
		clsPrevY.assign(NPART, 0.0f);
		clsPrevType.assign(NPART, 0);
		clsPrevParallel.assign(NPART, 0);
		clsMispredictByType.assign(PT_NUM, 0);
		clsMispredictMaxDist.assign(PT_NUM, 0.0f);
		clsMispredictAt30.assign(PT_NUM, 0);
		clsPrevSwapCount.assign(NPART, 0);
		clsPrevSwapDist.assign(NPART, 0.0f);
		swapCumCount.assign(NPART, 0);
		swapCumDist.assign(NPART, 0.0f);
		swapProbeEnabled = true;
	}
	// * Zera por frame: o que interessa e a composicao de um frame, nao o acumulado.
	clsParallel = 0; clsSerialType = 0; clsSerialMove = 0; clsMispredict = 0;
	Defer resetSegment([this]() { parts.SetCurrentSegment(0); });
	//the main particle loop function, goes over all particles.
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	costProbe.BeginLoop();
	for (auto i = start; i < end && i < parts.active; i++)
	{
		auto t = parts[i].type;
		costProbe.EnterSlot(t, elements);
		if (!t)
		{
			continue;
		}
		// * Fluxo de aleatorios proprio desta particula neste tick. O laco compartilhava um
		//   unico RNG, entao os numeros que cada particula recebia dependiam de quantas
		//   chamadas as anteriores tinham feito, isto e, da ordem de visita. Sob threads
		//   isso e corrida de dados e destroi a reprodutibilidade.
		//   Semeando por (tick, indice) o fluxo passa a ser funcao apenas da identidade da
		//   particula: o resultado deixa de depender da ordem de visita, do particionamento
		//   e ate do numero de threads.
		rng.seedFrom(uint64_t(currentTick), uint64_t(i));
		// * Ainda em execucao serial, mas atribuindo o segmento da lista livre pela faixa
		//   vertical em que a particula esta, que e exatamente o que o estagio 4 fara com
		//   uma thread por faixa. Sem isto a segmentacao nunca sai do segmento 0 e o codigo
		//   novo passa no teste sem nunca ter sido exercitado: em particular o resgate entre
		//   segmentos, que so dispara quando uma faixa libera mais do que aloca.
		if (freeListBanding)
		{
			auto band = int(parts[i].x) * parts.FreeSegments() / XRES;
			parts.SetCurrentSegment(std::max(0, std::min(band, parts.FreeSegments() - 1)));
		}
		if (classifyEnabled)
		{
			// * Confere primeiro o palpite do frame anterior: quanto esta particula andou de
			//   fato desde a ultima classificacao. Se tinha sido dada como paralela e andou
			//   mais que o halo, o preditor falhou de um jeito que corromperia estado numa
			//   execucao paralela de verdade. E este numero, e nao a fracao serial, que diz
			//   se a classificacao pode ser confiada.
			if (clsPrevType[i] == t)
			{
				auto moved = std::max(std::fabs(parts[i].x - clsPrevX[i]), std::fabs(parts[i].y - clsPrevY[i]));
				if (moved > DecompHalo && clsPrevParallel[i])
				{
					clsMispredict += 1;
					// * Registra por elemento. A janela de 29,5 a 30,5 isola a assinatura da
					//   busca lateral de liquidos, cujo limite e exatamente rt = 30.
					clsMispredictByType[t] += 1;
					clsMispredictMaxDist[t] = std::max(clsMispredictMaxDist[t], moved);
					if (moved > 29.5f && moved < 30.5f)
					{
						clsMispredictAt30[t] += 1;
					}
					// * Trocas sofridas por esta particula desde a ultima classificacao, ou
					//   seja, durante o frame que produziu este salto.
					auto swaps = swapCumCount[i] - clsPrevSwapCount[i];
					auto swapDist = swapCumDist[i] - clsPrevSwapDist[i];
					mispredictSwapTotal += swaps;
					mispredictMaxSwaps = std::max(mispredictMaxSwaps, swaps);
					mispredictDistSum += moved;
					mispredictSwapDistSum += swapDist;
					if (swaps > 0)
					{
						mispredictWithSwap += 1;
					}
					else
					{
						mispredictNoSwap += 1;
					}
				}
			}
			// * Preditor conservador: velocidade corrente em Chebyshev, com folga de um frame
			//   de aceleracao, mais o raio de varredura do tipo. Usa a velocidade de entrada
			//   porque a classificacao teria de acontecer antes do update numa execucao
			//   paralela; e justamente por isso que ela pode errar, e por isso que o
			//   contador acima existe.
			// * O preditor anterior usava so a velocidade de entrada e errava feio: a
			//   aceleracao que arremessa materia vem da ADVECCAO pelo grid de ar, aplicada
			//   dentro do mesmo frame. Como o grid de ar e resolvido em BeforeSim, antes
			//   deste laco, o termo e conhecido aqui e entra na previsao.
			//   Limite: |v|*Loss + |Advection * v_ar| + margem de gravidade, em Chebyshev.
			auto cx = std::max(0, std::min(int(parts[i].x + 0.5f), XRES - 1)) / CELL;
			auto cy = std::max(0, std::min(int(parts[i].y + 0.5f), YRES - 1)) / CELL;
			auto adv = std::fabs(sd.elements[t].Advection);
			auto predVx = std::fabs(parts[i].vx) + adv * std::fabs(vx[cy][cx]);
			auto predVy = std::fabs(parts[i].vy) + adv * std::fabs(vy[cy][cx]);
			auto speed = std::max(predVx, predVy);
			bool serialByType = IsUnboundedReader(t);
			bool serialByMove = (speed * 1.5f + 4.0f) > DecompHalo;
			if (serialByType)
			{
				clsSerialType += 1;
			}
			else if (serialByMove)
			{
				clsSerialMove += 1;
			}
			else
			{
				clsParallel += 1;
			}
			clsPrevX[i] = parts[i].x;
			clsPrevY[i] = parts[i].y;
			clsPrevType[i] = t;
			clsPrevParallel[i] = (!serialByType && !serialByMove) ? 1 : 0;
			clsPrevSwapCount[i] = swapCumCount[i];
			clsPrevSwapDist[i] = swapCumDist[i];
		}
		debug_mostRecentlyUpdated = i;

		auto x = int(parts[i].x+0.5f);
		auto y = int(parts[i].y+0.5f);

		// Kill a particle off screen
		if (x<CELL || y<CELL || x>=XRES-CELL || y>=YRES-CELL)
		{
			kill_part(i);
			continue;
		}

		// Kill a particle in a wall where it isn't supposed to go
		if (bmap[y/CELL][x/CELL] &&
		   (bmap[y/CELL][x/CELL]==WL_WALL ||
		    bmap[y/CELL][x/CELL]==WL_WALLELEC ||
		    bmap[y/CELL][x/CELL]==WL_ALLOWAIR ||
		    (bmap[y/CELL][x/CELL]==WL_DESTROYALL) ||
		    (bmap[y/CELL][x/CELL]==WL_ALLOWLIQUID && !(elements[t].Properties&TYPE_LIQUID)) ||
		    (bmap[y/CELL][x/CELL]==WL_ALLOWPOWDER && !(elements[t].Properties&TYPE_PART)) ||
		    (bmap[y/CELL][x/CELL]==WL_ALLOWGAS && !(elements[t].Properties&TYPE_GAS)) || //&& elements[t].Falldown!=0 && parts[i].type!=PT_FIRE && parts[i].type!=PT_SMKE && parts[i].type!=PT_CFLM) ||
		            (bmap[y/CELL][x/CELL]==WL_ALLOWENERGY && !(elements[t].Properties&TYPE_ENERGY)) ||
		    (bmap[y/CELL][x/CELL]==WL_EWALL && !emap[y/CELL][x/CELL])) && (t!=PT_STKM) && (t!=PT_STKM2) && (t!=PT_FIGH))
		{
			kill_part(i);
			continue;
		}

		// Make sure that STASIS'd particles don't tick.
		if (bmap[y/CELL][x/CELL] == WL_STASIS && emap[y/CELL][x/CELL]<8) {
			continue;
		}

		if (bmap[y/CELL][x/CELL]==WL_DETECT && emap[y/CELL][x/CELL]<8)
			set_emap(x/CELL, y/CELL);

		//adding to velocity from the particle's velocity
		vx[y/CELL][x/CELL] = vx[y/CELL][x/CELL]*elements[t].AirLoss + elements[t].AirDrag*parts[i].vx;
		vy[y/CELL][x/CELL] = vy[y/CELL][x/CELL]*elements[t].AirLoss + elements[t].AirDrag*parts[i].vy;

		if (elements[t].HotAir)
		{
			if (t==PT_GAS||t==PT_NBLE)
			{
				if (pv[y/CELL][x/CELL]<3.5f)
					pv[y/CELL][x/CELL] += 4.0f*elements[t].HotAir*(3.5f-pv[y/CELL][x/CELL]);
			}
			else//add the hotair variable to the pressure map, like black hole, or white hole.
			{
				pv[y/CELL][x/CELL] += 4.0f*elements[t].HotAir;
			}
		}

		auto neighbourhood = GetNeighbourhood(i);

		//velocity updates for the particle
		if (t != PT_SPNG || !(parts[i].flags&FLAG_MOVABLE))
		{
			parts[i].vx *= elements[t].Loss;
			parts[i].vy *= elements[t].Loss;
		}
		//particle gets velocity from the vx and vy maps
		parts[i].vx += elements[t].Advection*vx[y/CELL][x/CELL] + neighbourhood.pGravX;
		parts[i].vy += elements[t].Advection*vy[y/CELL][x/CELL] + neighbourhood.pGravY;


		if (elements[t].Diffusion)//the random diffusion that gasses have
		{
			parts[i].vx += elements[t].Diffusion*(2.0f*rng.uniform01()-1.0f);
			parts[i].vy += elements[t].Diffusion*(2.0f*rng.uniform01()-1.0f);
		}

		auto transitionOccurred = TransitionPhase(i, neighbourhood);
		if (!parts[i].type)
		{
			continue;
		}
		if (transitionOccurred)
		{
			t = parts[i].type;
		}

		//call the particle update function, if there is one
		if (elements[t].Update)
		{
			costProbe.RecordElementUpdate();
			if ((*(elements[t].Update))(this, i, x, y, neighbourhood.surround_space, neighbourhood.nt, parts, pmap))
				continue;
			x = int(parts[i].x+0.5f);
			y = int(parts[i].y+0.5f);
		}

		if(legacy_enable)//if heat sim is off
			Element::legacyUpdate(this, i,x,y,neighbourhood.surround_space,neighbourhood.nt, parts, pmap);

		if (parts[i].type == PT_NONE)//if its dead, skip to next particle
			continue;

		if (transitionOccurred)
			continue;

		if (!parts[i].vx&&!parts[i].vy)//if its not moving, skip to next particle, movement code it next
			continue;

		if (costProbe.ShouldSampleMovement(i, parts[i].type, elements))
		{
			// Keep the measurement boundary at the call site. Helper returns and sampling
			// branches must not be multiplied by the sampling stride as movement work.
			auto movementStartedAt = ParticleCostClock::now();
			MovementPhase(i, neighbourhood);
			auto movementEndedAt = ParticleCostClock::now();
			costProbe.RecordMovement(movementEndedAt - movementStartedAt);
		}
		else
		{
			MovementPhase(i, neighbourhood);
		}
	}
	costProbe.EndLoop();
}

bool SimulationImpl::TransitionPhase(int i, const Neighbourhood &neighbourhood)
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;

	auto t = parts[i].type;
	auto x = int(parts[i].x + 0.5f);
	auto y = int(parts[i].y + 0.5f);
	bool transitionOccurred = false;
	if (!legacy_enable)
	{
		float gel_scale = 1.0f;
		if (t==PT_GEL)
			gel_scale = parts[i].tmp*2.55f;

		if ((elements[t].Properties&TYPE_LIQUID) && (t!=PT_GEL || gel_scale > (1 + rng.between(0, 254))))
		{
			float convGravX, convGravY;
			GetGravityField(x, y, -2.0f, -2.0f, convGravX, convGravY);
			auto offsetX = std::clamp(int(std::round(convGravX + x)), x-1, x+1);
			auto offsetY = std::clamp(int(std::round(convGravY + y)), y-1, y+1);
			// Some heat convection for liquids
			if (offsetX != x || offsetY != y)
			{
				auto r = pmap[offsetY][offsetX];
				if (r && parts[i].type == TYP(r))
				{
					if (parts[i].temp>parts[ID(r)].temp)
					{
						auto swappage = parts[i].temp;
						parts[i].temp = parts[ID(r)].temp;
						parts[ID(r)].temp = swappage;
					}
				}
			}
		}

		// Heat transfer code
		if (t && !sd.IsHeatInsulator(parts[i]) && rng.chance(int(elements[t].HeatConduct*gel_scale), 250))
		{
			// Heat transfer with air
			if (aheat_enable && !(elements[t].Properties&PROP_NOAMBHEAT))
			{
				auto dtemp = hv[y/CELL][x/CELL] - parts[i].temp; // Temperature difference
				auto hc = sd.HeatCapacityOf(parts[i]);
				auto alpha = std::min(0.04f, 0.4f * hc); // alpha / heat_capacity must be < 1

				// Here we completely ignore that there are CELL^2 "air pixels" in a cell, and the heat capacity of air
				parts[i].temp = restrict_flt(parts[i].temp + alpha*dtemp / hc, MIN_TEMP, MAX_TEMP);
				hv[y/CELL][x/CELL] = restrict_flt(hv[y/CELL][x/CELL] - alpha*dtemp, MIN_TEMP, MAX_TEMP);
			}

			// Heat transfer with other elements
			auto hc_total = 0.0f; // Total heat capacity of elements involved
			auto c_heat = 0.0f; // Total heat distributed between elements
			int surround_hconduct[8]; // IDs of elements which exchange heat

			for (auto j=0; j<8; j++)
			{
				surround_hconduct[j] = i;
				auto r = neighbourhood.surround[j];

				if (!r)
					continue;

				auto rt = TYP(r);

				// Check if we can conduct heat
				if (!rt || sd.IsHeatInsulator(parts[ID(r)])
				        || (t == PT_FILT && (rt == PT_BRAY || rt == PT_BIZR || rt == PT_BIZRG))
				        || (rt == PT_FILT && (t == PT_BRAY || t == PT_PHOT || t == PT_BIZR || t == PT_BIZRG))
				        || (t == PT_ELEC && rt == PT_DEUT)
				        || (t == PT_DEUT && rt == PT_ELEC)
				        || (t == PT_HSWC && rt == PT_FILT && parts[i].tmp == 1)
				        || (t == PT_FILT && rt == PT_HSWC && parts[ID(r)].tmp == 1))
					continue;

				surround_hconduct[j] = ID(r);
				auto hc = sd.HeatCapacityOf(parts[ID(r)]);
				c_heat += parts[ID(r)].temp*hc;
				hc_total += hc;
			}

			// Add the current particle
			auto hc = sd.HeatCapacityOf(parts[i]);
			c_heat += parts[i].temp*hc;
			hc_total += hc;

			// Equilibrium temperature
			float pt = restrict_flt(c_heat / hc_total, MIN_TEMP, MAX_TEMP);

			parts[i].temp = pt;
			for (auto j=0; j<8; j++)
			{
				parts[surround_hconduct[j]].temp = pt;
			}

			auto ctemph = pt;
			auto ctempl = pt;
			// change boiling point with pressure
			if (((elements[t].Properties&TYPE_LIQUID) && sd.IsElementOrNone(elements[t].HighTemperatureTransition) && (elements[elements[t].HighTemperatureTransition].Properties&TYPE_GAS))
			        || t==PT_LNTG || t==PT_SLTW)
				ctemph -= 2.0f*pv[y/CELL][x/CELL];
			else if (((elements[t].Properties&TYPE_GAS) && sd.IsElementOrNone(elements[t].LowTemperatureTransition) && (elements[elements[t].LowTemperatureTransition].Properties&TYPE_LIQUID))
			         || t==PT_WTRV)
				ctempl -= 2.0f*pv[y/CELL][x/CELL];
			auto s = 1;

			//A fix for ice with ctype = 0
			if ((t==PT_ICEI || t==PT_SNOW) && (!sd.IsElement(parts[i].ctype) || parts[i].ctype==PT_ICEI || parts[i].ctype==PT_SNOW))
				parts[i].ctype = PT_WATR;

			if (elements[t].HighTemperatureTransition != NT && ctemph>=elements[t].HighTemperature)
			{
				// particle type change due to high temperature
				if (elements[t].HighTemperatureTransition != ST)
				{
					if (t == PT_FOG)
						parts[i].ctype = 0; // clear unnecessary ctype
					t = elements[t].HighTemperatureTransition;
				}
				else if (t == PT_ICEI || t == PT_SNOW)
				{
					if (parts[i].ctype > 0 && parts[i].ctype < PT_NUM && parts[i].ctype != t)
					{
						if (elements[parts[i].ctype].LowTemperatureTransition==PT_ICEI || elements[parts[i].ctype].LowTemperatureTransition==PT_SNOW)
						{
							if (pt<elements[parts[i].ctype].LowTemperature)
								s = 0;
						}
						else if (pt<273.15f)
							s = 0;

						if (s)
						{
							t = parts[i].ctype;
							parts[i].ctype = PT_NONE;
							parts[i].life = 0;
						}
					}
					else
						s = 0;
				}
				else if (t == PT_SLTW)
				{
					//@ SLTW -> SALT/WTRV
					t = rng.chance(1, 4) ? PT_SALT : PT_WTRV;
				}
				else if (t == PT_BRMT)
				{
					//@ BRMT(TUNG) -> LAVA(TUNG)
					if (parts[i].ctype == PT_TUNG)
					{
						if (ctemph < elements[parts[i].ctype].HighTemperature)
							s = 0;
						else
						{
							t = PT_LAVA;
							parts[i].type = PT_TUNG;
						}
					}
					else if (ctemph >= elements[t].HighTemperature)
						t = PT_LAVA;
					else
						s = 0;
				}
				else if (t == PT_CRMC)
				{
					float pres = std::max((pv[y/CELL][x/CELL]+pv[(y-2)/CELL][x/CELL]+pv[(y+2)/CELL][x/CELL]+pv[y/CELL][(x-2)/CELL]+pv[y/CELL][(x+2)/CELL])*2.0f, 0.0f);
					if (ctemph < pres+elements[PT_CRMC].HighTemperature)
						s = 0;
					else
						t = PT_LAVA;
				}
				else if (t == PT_RIME)
				{
					if (parts[i].tmp > 5)
					{
						//@ RIME -> ACID
						t = PT_ACID;
						parts[i].life = 25 + 5 * parts[i].tmp;
						parts[i].tmp = 0;
					}
					else
					{
						//@ RIME -> WATR
						t = parts[i].ctype == PT_DSTW ? PT_DSTW : PT_WATR;
						parts[i].ctype = 0;
					}
				}
				else
					s = 0;
			}
			else if (elements[t].LowTemperatureTransition != NT && ctempl<elements[t].LowTemperature)
			{
				// particle type change due to low temperature
				if (elements[t].LowTemperatureTransition != ST)
				{
					t = elements[t].LowTemperatureTransition;
				}
				else if (t == PT_WTRV)
				{
					//@ WTRV -> RIME/DSTW
					t = (pt < 273.0f) ? PT_RIME : PT_DSTW;
				}
				else if (t == PT_LAVA)
				{
					if (parts[i].ctype > 0 && parts[i].ctype < PT_NUM && parts[i].ctype != PT_LAVA && elements[parts[i].ctype].Enabled)
					{
						if (parts[i].ctype == PT_THRM && pt >= elements[PT_BMTL].HighTemperature)
							s = 0;
						else if ((parts[i].ctype == PT_VIBR || parts[i].ctype == PT_BVBR) && pt >= 273.15f)
							s = 0;
						else if (parts[i].ctype == PT_TUNG)
						{
							// TUNG does its own melting in its update function, so HighTemperatureTransition is not LAVA so it won't be handled by the code for HighTemperatureTransition==PT_LAVA below
							// However, the threshold is stored in HighTemperature to allow it to be changed from Lua
							if (pt >= elements[parts[i].ctype].HighTemperature)
								s = 0;
						}
						else if (parts[i].ctype == PT_CRMC)
						{
							float pres = std::max((pv[y/CELL][x/CELL]+pv[(y-2)/CELL][x/CELL]+pv[(y+2)/CELL][x/CELL]+pv[y/CELL][(x-2)/CELL]+pv[y/CELL][(x+2)/CELL])*2.0f, 0.0f);
							if (ctemph >= pres+elements[PT_CRMC].HighTemperature)
								s = 0;
						}
						else if (elements[parts[i].ctype].HighTemperatureTransition == PT_LAVA || parts[i].ctype == PT_HEAC)
						{
							if (pt >= elements[parts[i].ctype].HighTemperature)
								s = 0;
						}
						else if (pt>=973.0f)
							s = 0; // freezing point for lava with any other (not listed in ptransitions as turning into lava) ctype
						if (s)
						{
							t = parts[i].ctype;
							parts[i].ctype = PT_NONE;
							if (t == PT_THRM)
							{
								//@ LAVA(THRM) -> BMTL
								parts[i].tmp = 0;
								t = PT_BMTL;
							}
							if (t == PT_PLUT)
							{
								//@ LAVA(PLUT) -> LAVA
								parts[i].tmp = 0;
								t = PT_LAVA;
							}
						}
					}
					else if (pt<973.0f)
						t = PT_STNE; //@ LAVA -> STNE
					else
						s = 0;
				}
				else
					s = 0;
			}
			else
				s = 0;

			if (s) // particle type change occurred
			{
				if (t==PT_ICEI || t==PT_LAVA || t==PT_SNOW)
					parts[i].ctype = parts[i].type;
				if (t == PT_RIME)
					parts[i].ctype = PT_DSTW;
				if (!(t==PT_ICEI && parts[i].ctype==PT_FRZW) && t!=PT_ACID)
					parts[i].life = 0;
				if (t == PT_FIRE)
				{
					//hackish, if tmp isn't 0 the FIRE might turn into DSTW later
					//idealy transitions should use create_part(i) but some elements rely on properties staying constant
					//and I don't feel like checking each one right now
					parts[i].tmp = 0;

					if (parts[i].type == PT_SEED)
					{
						parts[i].ctype = 0;
						parts[i].tmp2 = 0;
						parts[i].tmp3 = 0;
						parts[i].tmp4 = 0;
					}
				}
				if ((elements[t].Properties&TYPE_GAS) && !(elements[parts[i].type].Properties&TYPE_GAS))
					pv[y/CELL][x/CELL] += 0.50f;

				if (t == PT_NONE)
				{
					kill_part(i);
					return true;
				}
				// part_change_type could refuse to change the type and kill the particle
				// for example, changing type to STKM but one already exists
				// we need to account for that to not cause simulation corruption issues
				if (part_change_type(i,x,y,t))
					return true;

				if (t==PT_FIRE || t==PT_PLSM || t==PT_CFLM)
					parts[i].life = rng.between(120, 169);
				if (t == PT_LAVA)
				{
					if (parts[i].ctype == PT_BRMT) parts[i].ctype = PT_BMTL;
					else if (parts[i].ctype == PT_SAND) parts[i].ctype = PT_GLAS;
					else if (parts[i].ctype == PT_BGLA) parts[i].ctype = PT_GLAS;
					else if (parts[i].ctype == PT_PQRT) parts[i].ctype = PT_QRTZ;
					else if (parts[i].ctype == PT_LITH && parts[i].tmp2 > 3) parts[i].ctype = PT_GLAS;
					parts[i].life = rng.between(240, 359);
				}
				transitionOccurred = true;
			}

			pt = parts[i].temp = restrict_flt(parts[i].temp, MIN_TEMP, MAX_TEMP);
			if (t == PT_LAVA)
			{
				parts[i].life = int(restrict_flt((parts[i].temp-700)/7, 0, 400));
				if (parts[i].ctype==PT_THRM&&parts[i].tmp>0)
				{
					parts[i].tmp--;
					parts[i].temp = 3500;
				}
				if (parts[i].ctype==PT_PLUT&&parts[i].tmp>0)
				{
					parts[i].tmp--;
					parts[i].temp = MAX_TEMP;
				}
			}
		}
		else
		{
			if (!(air->bmap_blockairh[y/CELL][x/CELL]&0x8))
				air->bmap_blockairh[y/CELL][x/CELL]++;
			parts[i].temp = restrict_flt(parts[i].temp, MIN_TEMP, MAX_TEMP);
		}
	}

	if (t==PT_LIFE)
	{
		parts[i].temp = restrict_flt(parts[i].temp-50.0f, MIN_TEMP, MAX_TEMP);
	}
	//spark updates from walls
	if ((elements[t].Properties&PROP_CONDUCTS) || t==PT_SPRK)
	{
		auto nx = x % CELL;
		if (nx == 0)
			nx = x/CELL - 1;
		else if (nx == CELL-1)
			nx = x/CELL + 1;
		else
			nx = x/CELL;
		auto ny = y % CELL;
		if (ny == 0)
			ny = y/CELL - 1;
		else if (ny == CELL-1)
			ny = y/CELL + 1;
		else
			ny = y/CELL;
		if (nx>=0 && ny>=0 && nx<XCELLS && ny<YCELLS)
		{
			if (t!=PT_SPRK)
			{
				if (emap[ny][nx]==12 && !parts[i].life && bmap[ny][nx] != WL_STASIS)
				{
					part_change_type(i,x,y,PT_SPRK);
					parts[i].life = 4;
					parts[i].ctype = t;
					t = PT_SPRK;
				}
			}
			else if (bmap[ny][nx]==WL_DETECT || bmap[ny][nx]==WL_EWALL || bmap[ny][nx]==WL_ALLOWLIQUID || bmap[ny][nx]==WL_WALLELEC || bmap[ny][nx]==WL_ALLOWALLELEC || bmap[ny][nx]==WL_EHOLE)
				set_emap(nx, ny);
		}
	}

	//the basic explosion, from the .explosive variable
	if ((elements[t].Explosive&2) && pv[y/CELL][x/CELL]>2.5f)
	{
		parts[i].life = rng.between(180, 259);
		parts[i].temp = restrict_flt(elements[PT_FIRE].DefaultProperties.temp + (elements[t].Flammable/2), MIN_TEMP, MAX_TEMP);
		t = PT_FIRE;
		part_change_type(i,x,y,t);
		pv[y/CELL][x/CELL] += 0.25f * CFDS;
	}

	{
		auto s = 1;
		auto gravtot = std::abs(gravOut.forceX[Vec2{ x, y } / CELL]) +
		               std::abs(gravOut.forceY[Vec2{ x, y } / CELL]);
		if (elements[t].HighPressureTransition != NT && pv[y/CELL][x/CELL]>elements[t].HighPressure) {
			// particle type change due to high pressure
			if (elements[t].HighPressureTransition != ST)
				t = elements[t].HighPressureTransition;
			else if (t==PT_BMTL) {
				if (pv[y/CELL][x/CELL]>2.5f)
					t = PT_BRMT;
				else if (pv[y/CELL][x/CELL]>1.0f && parts[i].tmp==1)
					t = PT_BRMT;
				else s = 0;
			}
			else s = 0;
		} else if (elements[t].LowPressureTransition != NT && pv[y/CELL][x/CELL]<elements[t].LowPressure && gravtot<=(elements[t].LowPressure/4.0f)) {
			// particle type change due to low pressure
			if (elements[t].LowPressureTransition != ST)
				t = elements[t].LowPressureTransition;
			else s = 0;
		} else if (elements[t].HighPressureTransition != NT && gravtot>(elements[t].HighPressure/4.0f)) {
			// particle type change due to high gravity
			if (elements[t].HighPressureTransition != ST)
				t = elements[t].HighPressureTransition;
			else if (t==PT_BMTL) {
				if (gravtot>0.625f)
					t = PT_BRMT;
				else if (gravtot>0.25f && parts[i].tmp==1)
					t = PT_BRMT;
				else s = 0;
			}
			else s = 0;
		} else s = 0;

		// particle type change occurred
		if (s)
		{
			if (t == PT_NONE)
			{
				kill_part(i);
				return true;
			}
			parts[i].life = 0;

			// To prevent PIPE -> BRMT setting BRMT's ctype
			if (t == PT_BRMT)
			{
				parts[i].ctype = 0;
				parts[i].tmp = 0;
				parts[i].tmp2 = 0;
				parts[i].tmp3 = 0;
				parts[i].tmp4 = 0;
			}

			// part_change_type could refuse to change the type and kill the particle
			// for example, changing type to STKM but one already exists
			// we need to account for that to not cause simulation corruption issues
			if (part_change_type(i,x,y,t))
				return true;
			if (t == PT_FIRE)
				parts[i].life = rng.between(120, 169);
			transitionOccurred = true;
		}
	}
	return transitionOccurred;
}

void SimulationImpl::MovementPhase(int i, Neighbourhood neighbourhood)
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;

	auto t = parts[i].type;
	auto x = int(parts[i].x+0.5f);
	auto y = int(parts[i].y+0.5f);
	int fin_x, fin_y, clear_x, clear_y;
	float fin_xf, fin_yf, clear_xf, clear_yf;
	{
		auto mr = PlanMove<true>(*this, i, x, y);
		fin_x    = mr.fin_x;
		fin_y    = mr.fin_y;
		clear_x  = mr.clear_x;
		clear_y  = mr.clear_y;
		fin_xf   = mr.fin_xf;
		fin_yf   = mr.fin_yf;
		clear_xf = mr.clear_xf;
		clear_yf = mr.clear_yf;
		parts[i].vx = mr.vx;
		parts[i].vy = mr.vy;
	}

	auto stagnant = parts[i].flags & FLAG_STAGNANT;
	parts[i].flags &= ~FLAG_STAGNANT;

	if (t==PT_STKM || t==PT_STKM2 || t==PT_FIGH)
	{
		//head movement, let head pass through anything
		parts[i].x += parts[i].vx;
		parts[i].y += parts[i].vy;
		int nx = (int)((float)parts[i].x+0.5f);
		int ny = (int)((float)parts[i].y+0.5f);
		if (edgeMode == EDGE_LOOP)
		{
			bool x_ok = (nx >= CELL && nx < XRES-CELL);
			bool y_ok = (ny >= CELL && ny < YRES-CELL);
			int oldnx = nx, oldny = ny;
			if (!x_ok)
			{
				parts[i].x = remainder_p(parts[i].x-CELL+.5f, XRES-CELL*2.0f)+CELL-.5f;
				nx = (int)((float)parts[i].x+0.5f);
			}
			if (!y_ok)
			{
				parts[i].y = remainder_p(parts[i].y-CELL+.5f, YRES-CELL*2.0f)+CELL-.5f;
				ny = (int)((float)parts[i].y+0.5f);
			}

			if (!x_ok || !y_ok) //when moving from left to right stickmen might be able to fall through solid things, fix with "eval_move(t, nx+diffx, ny+diffy, NULL)" but then they die instead
			{
				//adjust stickmen legs
				playerst* stickman = nullptr;
				int t = parts[i].type;
				if (t == PT_STKM)
					stickman = &player;
				else if (t == PT_STKM2)
					stickman = &player2;
				else if (t == PT_FIGH && parts[i].tmp >= 0 && parts[i].tmp < MAX_FIGHTERS)
					stickman = &fighters[parts[i].tmp];

				if (stickman)
					for (int i = 0; i < 16; i+=2)
					{
						stickman->legs[i] += (nx-oldnx);
						stickman->legs[i+1] += (ny-oldny);
						stickman->accs[i/2] *= .95f;
					}
				parts[i].vy *= .95f;
				parts[i].vx *= .95f;
			}
		}
		if (ny!=y || nx!=x)
		{
			if (pmap[y][x] && ID(pmap[y][x]) == i)
				pmap[y][x] = 0;
			else if (photons[y][x] && ID(photons[y][x]) == i)
				photons[y][x] = 0;
			if (nx<CELL || nx>=XRES-CELL || ny<CELL || ny>=YRES-CELL)
			{
				kill_part(i);
				return;
			}
			if (elements[t].Properties & TYPE_ENERGY)
				photons[ny][nx] = PMAP(i, t);
			else if (t)
				pmap[ny][nx] = PMAP(i, t);
		}
	}
	else if (elements[t].Properties & TYPE_ENERGY)
	{
		if (t == PT_PHOT)
		{
			if (parts[i].flags&FLAG_SKIPMOVE)
			{
				parts[i].flags &= ~FLAG_SKIPMOVE;
				return;
			}

			if (eval_move(PT_PHOT, fin_x, fin_y, nullptr))
			{
				int rt = TYP(pmap[fin_y][fin_x]);
				int lt = TYP(pmap[y][x]);
				int rt_glas = (rt == PT_GLAS) || (rt == PT_BGLA);
				int lt_glas = (lt == PT_GLAS) || (lt == PT_BGLA);
				if ((rt_glas && !lt_glas) || (lt_glas && !rt_glas))
				{
					auto gn = get_normal_interp<true>(*this, REFRACT|t, parts[i].x, parts[i].y, parts[i].vx, parts[i].vy);
					if (!gn.success) {
						kill_part(i);
						return;
					}
					auto nrx = gn.nx;
					auto nry = gn.ny;
					auto r = get_wavelength_bin(&parts[i].ctype);
					if (r == -1 || !(parts[i].ctype&0x3FFFFFFF))
					{
						kill_part(i);
						return;
					}
					auto nn = GLASS_IOR - GLASS_DISP*(r-30)/30.0f;
					nn *= nn;

					auto enter = rt_glas && !lt_glas;
					nrx = enter ? -nrx : nrx;
					nry = enter ? -nry : nry;
					nn = enter ? 1.0f/nn : nn;
					auto ct1 = parts[i].vx*nrx + parts[i].vy*nry;
					auto ct2 = 1.0f - (nn*nn)*(1.0f-(ct1*ct1));
					if (ct2 < 0.0f) {
						// total internal reflection
						parts[i].vx -= 2.0f*ct1*nrx;
						parts[i].vy -= 2.0f*ct1*nry;
						fin_xf = parts[i].x;
						fin_yf = parts[i].y;
						fin_x = x;
						fin_y = y;
					} else {
						// refraction
						ct2 = sqrtf(ct2);
						ct2 = ct2 - nn*ct1;
						parts[i].vx = nn*parts[i].vx + ct2*nrx;
						parts[i].vy = nn*parts[i].vy + ct2*nry;
					}
				}
			}
		}
		if (stagnant)//FLAG_STAGNANT set, was reflected on previous frame
		{
			// cast coords as int then back to float for compatibility with existing saves
			if (!do_move(i, x, y, (float)fin_x, (float)fin_y) && parts[i].type) {
				kill_part(i);
				return;
			}
		}
		else if (!do_move(i, x, y, fin_xf, fin_yf))
		{
			if (parts[i].type == PT_NONE)
				return;
			// reflection
			parts[i].flags |= FLAG_STAGNANT;
			if (t==PT_NEUT && rng.chance(1, 10))
			{
				kill_part(i);
				return;
			}
			auto r = pmap[fin_y][fin_x];

			if ((TYP(r)==PT_PIPE || TYP(r) == PT_PPIP) && !TYP(parts[ID(r)].ctype))
			{
				Element_PIPE_transfer_part_to_pipe(parts+i, parts+(ID(r)));
				return;
			}

			if (t == PT_PHOT)
			{
				auto mask = elements[TYP(r)].PhotonReflectWavelengths;
				if (TYP(r) == PT_LITH)
				{
					int wl_bin = parts[ID(r)].ctype / 4;
					if (wl_bin < 0) wl_bin = 0;
					if (wl_bin > 25) wl_bin = 25;
					mask = (0x1F << wl_bin);
				}
				else if (TYP(r) == PT_SEED)
				{
					// Reflect different wavelengths based on SEED's color genes
					int colour = (parts[ID(r)].ctype >> PLNT_COLOUR) & 0x3f;

					mask |= ((colour & 0b110000) != 0) ? 0 : (1 << 25); // Red
					mask |= ((colour & 0b001100) != 0) ? 0 : (1 << 15); // Green
					mask |= ((colour & 0b000011) != 0) ? 0 : (1 << 5); // Blue

				}
				parts[i].ctype &= mask;
			}

			auto gn = get_normal_interp<true>(*this, t, parts[i].x, parts[i].y, parts[i].vx, parts[i].vy);
			if (gn.success)
			{
				auto nrx = gn.nx;
				auto nry = gn.ny;
				if (TYP(r) == PT_CRMC)
				{
					float r = rng.between(-50, 50) * 0.01f, rx, ry, anrx, anry;
					r = r * r * r;
					rx = cosf(r); ry = sinf(r);
					anrx = rx * nrx + ry * nry;
					anry = rx * nry - ry * nrx;
					auto dp = anrx*parts[i].vx + anry*parts[i].vy;
					parts[i].vx -= 2.0f*dp*anrx;
					parts[i].vy -= 2.0f*dp*anry;
				}
				else
				{
					auto dp = nrx*parts[i].vx + nry*parts[i].vy;
					parts[i].vx -= 2.0f*dp*nrx;
					parts[i].vy -= 2.0f*dp*nry;
				}
				// leave the actual movement until next frame so that reflection of fast particles and refraction happen correctly
			}
			else
			{
				if (t!=PT_NEUT)
					kill_part(i);
				return;
			}
			if (!(parts[i].ctype&0x3FFFFFFF) && t == PT_PHOT)
			{
				kill_part(i);
				return;
			}
		}
	}
	else if (elements[t].Falldown==0)
	{
		// gasses and solids (but not powders)
		if (!do_move(i, x, y, fin_xf, fin_yf))
		{
			if (parts[i].type == PT_NONE)
				return;
			// can't move there, so bounce off
			if (fin_x>x+ISTP) fin_x=x+ISTP;
			if (fin_x<x-ISTP) fin_x=x-ISTP;
			if (fin_y>y+ISTP) fin_y=y+ISTP;
			if (fin_y<y-ISTP) fin_y=y-ISTP;
			if (do_move(i, x, y, float(2*x-fin_x), float(fin_y)))
			{
				parts[i].vx *= elements[t].Collision;
			}
			else if (do_move(i, x, y, float(fin_x), float(2*y-fin_y)))
			{
				parts[i].vy *= elements[t].Collision;
			}
			else
			{
				parts[i].vx *= elements[t].Collision;
				parts[i].vy *= elements[t].Collision;
			}
		}
	}
	else
	{
		// Checking stagnant is cool, but then it doesn't update when you change it later.
		if (water_equal_test && elements[t].Falldown == 2 && rng.chance(1, 200))
		{
			if (flood_water(x, y, i))
				return;
		}
		// liquids and powders
		if (!do_move(i, x, y, fin_xf, fin_yf))
		{
			if (parts[i].type == PT_NONE)
				return;
			if (fin_x!=x && do_move(i, x, y, fin_xf, clear_yf))
			{
				parts[i].vx *= elements[t].Collision;
				parts[i].vy *= elements[t].Collision;
			}
			else if (fin_y!=y && do_move(i, x, y, clear_xf, fin_yf))
			{
				parts[i].vx *= elements[t].Collision;
				parts[i].vy *= elements[t].Collision;
			}
			else
			{
				auto pGravX = neighbourhood.pGravX;
				auto pGravY = neighbourhood.pGravY;
				auto r = rng.between(0, 1) * 2 - 1;// position search direction (left/right first)
				if ((clear_x!=x || clear_y!=y || neighbourhood.nt || neighbourhood.surround_space) &&
					(fabsf(parts[i].vx)>0.01f || fabsf(parts[i].vy)>0.01f))
				{
					// allow diagonal movement if target position is blocked
					// but no point trying this if particle is stuck in a block of identical particles
					auto dx = parts[i].vx - parts[i].vy*r;
					auto dy = parts[i].vy + parts[i].vx*r;

					auto mv = std::max(fabsf(dx), fabsf(dy));
					dx /= mv;
					dy /= mv;
					if (do_move(i, x, y, clear_xf+dx, clear_yf+dy))
					{
						parts[i].vx *= elements[t].Collision;
						parts[i].vy *= elements[t].Collision;
						return;
					}
					{
						auto swappage = dx;
						dx = dy*r;
						dy = -swappage*r;
					}
					if (do_move(i, x, y, clear_xf+dx, clear_yf+dy))
					{
						parts[i].vx *= elements[t].Collision;
						parts[i].vy *= elements[t].Collision;
						return;
					}
				}
				if (elements[t].Falldown>1 && !grav && gravityMode==GRAV_VERTICAL && parts[i].vy>fabsf(parts[i].vx))
				{
					if (particleCostCollecting)
					{
						particleCostWorking.lateralVerticalEntries += 1;
					}
					auto s = 0;
					// stagnant is true if FLAG_STAGNANT was set for this particle in previous frame
					int rt;
					if (!stagnant || neighbourhood.nt) //nt is if there is an something else besides the current particle type, around the particle
						rt = 30;//slight less water lag, although it changes how it moves a lot
					else
						rt = 10;

					if (t==PT_GEL)
						rt = int(parts[i].tmp*0.20f+5.0f);

					auto nx = -1, ny = -1;
					for (auto j=clear_x+r; j>=0 && j>=clear_x-rt && j<clear_x+rt && j<XRES; j+=r)
					{
						if (particleCostCollecting)
						{
							particleCostWorking.lateralSearchSteps += 1;
						}
						if ((TYP(pmap[fin_y][j])!=t || bmap[fin_y/CELL][j/CELL])
							&& (s=do_move(i, x, y, (float)j, fin_yf)))
						{
							nx = (int)(parts[i].x+0.5f);
							ny = (int)(parts[i].y+0.5f);
							break;
						}
						if (fin_y!=clear_y && (TYP(pmap[clear_y][j])!=t || bmap[clear_y/CELL][j/CELL])
							&& (s=do_move(i, x, y, (float)j, clear_yf)))
						{
							nx = (int)(parts[i].x+0.5f);
							ny = (int)(parts[i].y+0.5f);
							break;
						}
						if (TYP(pmap[clear_y][j])!=t || (bmap[clear_y/CELL][j/CELL] && bmap[clear_y/CELL][j/CELL]!=WL_STREAM))
							break;
					}

					r = (parts[i].vy>0) ? 1 : -1;

					if (s==1)
						for (auto j=ny+r; j>=0 && j<YRES && j>=ny-rt && j<ny+rt; j+=r)
						{
							if (particleCostCollecting)
							{
								particleCostWorking.lateralSearchSteps += 1;
							}
							if ((TYP(pmap[j][nx])!=t || bmap[j/CELL][nx/CELL]) && do_move(i, nx, ny, (float)nx, (float)j))
								break;
							if (TYP(pmap[j][nx])!=t || (bmap[j/CELL][nx/CELL] && bmap[j/CELL][nx/CELL]!=WL_STREAM))
								break;
						}
					else if (s==-1) {} // particle is out of bounds
					else if ((clear_x!=x||clear_y!=y) && do_move(i, x, y, clear_xf, clear_yf)) {}
					else parts[i].flags |= FLAG_STAGNANT;
					parts[i].vx *= elements[t].Collision;
					parts[i].vy *= elements[t].Collision;
				}
				else if (elements[t].Falldown>1 && fabsf(pGravX*parts[i].vx+pGravY*parts[i].vy)>fabsf(pGravY*parts[i].vx-pGravX*parts[i].vy))
				{
					if (particleCostCollecting)
					{
						particleCostWorking.lateralGravityEntries += 1;
					}
					float nxf, nyf, prev_pGravX, prev_pGravY, ptGrav = elements[t].Gravity;
					auto s = 0;
					// stagnant is true if FLAG_STAGNANT was set for this particle in previous frame
					// nt is if there is something else besides the current particle type around the particle
					// 30 gives slightly less water lag, although it changes how it moves a lot
					auto rt = (!stagnant || neighbourhood.nt) ? 30 : 10;

					// clear_xf, clear_yf is the last known position that the particle should almost certainly be able to move to
					nxf = clear_xf;
					nyf = clear_yf;
					auto nx = clear_x;
					auto ny = clear_y;
					// Look for spaces to move horizontally (perpendicular to gravity direction), keep going until a space is found or the number of positions examined = rt
					for (auto j=0;j<rt;j++)
					{
						if (particleCostCollecting)
						{
							particleCostWorking.lateralSearchSteps += 1;
						}
						// Calculate overall gravity direction
						GetGravityField(nx, ny, ptGrav, 1.0f, pGravX, pGravY);
						// Scale gravity vector so that the largest component is 1 pixel
						auto mv = std::max(fabsf(pGravX), fabsf(pGravY));
						if (mv<0.0001f) break;
						pGravX /= mv;
						pGravY /= mv;
						// Move 1 pixel perpendicularly to gravity
						// r is +1/-1, to try moving left or right at random
						if (j)
						{
							// Not quite the gravity direction
							// Gravity direction + last change in gravity direction
							// This makes liquid movement a bit less frothy, particularly for balls of liquid in radial gravity. With radial gravity, instead of just moving along a tangent, the attempted movement will follow the curvature a bit better.
							nxf += r*(pGravY*2.0f-prev_pGravY);
							nyf += -r*(pGravX*2.0f-prev_pGravX);
						}
						else
						{
							nxf += r*pGravY;
							nyf += -r*pGravX;
						}
						prev_pGravX = pGravX;
						prev_pGravY = pGravY;
						// Check whether movement is allowed
						nx = (int)(nxf+0.5f);
						ny = (int)(nyf+0.5f);
						if (nx<0 || ny<0 || nx>=XRES || ny >=YRES)
							break;
						if (TYP(pmap[ny][nx])!=t || bmap[ny/CELL][nx/CELL])
						{
							s = do_move(i, x, y, nxf, nyf);
							if (s)
							{
								// Movement was successful
								nx = (int)(parts[i].x+0.5f);
								ny = (int)(parts[i].y+0.5f);
								break;
							}
							// A particle of a different type, or a wall, was found. Stop trying to move any further horizontally unless the wall should be completely invisible to particles.
							if (TYP(pmap[ny][nx])!=t || bmap[ny/CELL][nx/CELL]!=WL_STREAM)
								break;
						}
					}
					if (s==1)
					{
						// The particle managed to move horizontally, now try to move vertically (parallel to gravity direction)
						// Keep going until the particle is blocked (by something that isn't the same element) or the number of positions examined = rt
						clear_x = nx;
						clear_y = ny;
						for (auto j=0;j<rt;j++)
						{
							if (particleCostCollecting)
							{
								particleCostWorking.lateralSearchSteps += 1;
							}
							// Calculate overall gravity direction
							GetGravityField(nx, ny, ptGrav, 1.0f, pGravX, pGravY);
							// Scale gravity vector so that the largest component is 1 pixel
							auto mv = std::max(fabsf(pGravX), fabsf(pGravY));
							if (mv<0.0001f) break;
							pGravX /= mv;
							pGravY /= mv;
							// Move 1 pixel in the direction of gravity
							nxf += pGravX;
							nyf += pGravY;
							nx = (int)(nxf+0.5f);
							ny = (int)(nyf+0.5f);
							if (nx<0 || ny<0 || nx>=XRES || ny>=YRES)
								break;
							// If the space is anything except the same element (a wall, empty space, or occupied by a particle of a different element), try to move into it
							if (TYP(pmap[ny][nx])!=t || bmap[ny/CELL][nx/CELL])
							{
								s = do_move(i, clear_x, clear_y, nxf, nyf);
								if (s || TYP(pmap[ny][nx])!=t || bmap[ny/CELL][nx/CELL]!=WL_STREAM)
									break; // found the edge of the liquid and movement into it succeeded, so stop moving down
							}
						}
					}
					else if (s==-1) {} // particle is out of bounds
					else if ((clear_x!=x||clear_y!=y) && do_move(i, x, y, clear_xf, clear_yf)) {} // try moving to the last clear position
					else parts[i].flags |= FLAG_STAGNANT;
					parts[i].vx *= elements[t].Collision;
					parts[i].vy *= elements[t].Collision;
				}
				else
				{
					// if interpolation was done, try moving to last clear position
					if ((clear_x!=x||clear_y!=y) && do_move(i, x, y, clear_xf, clear_yf)) {}
					else parts[i].flags |= FLAG_STAGNANT;
					parts[i].vx *= elements[t].Collision;
					parts[i].vy *= elements[t].Collision;
				}
			}
		}
	}
}

void Simulation::RecalcFreeParticles(bool do_life_dec)
{
	FrameTime::Span span(frameTime, "Simulation::RecalcFreeParticles");
	memset(pmap, 0, sizeof(pmap));
	memset(pmap_count, 0, sizeof(pmap_count));
	memset(photons, 0, sizeof(photons));

	NUM_PARTS = 0;
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	//the particle loop that resets the pmap/photon maps every frame, to update them.
	for (int i = 0; i < parts.active; i++)
	{
		if (!parts[i].type)
		{
			continue;
		}
		auto t = parts[i].type;
		auto x = int(parts[i].x+0.5f);
		auto y = int(parts[i].y+0.5f);
		bool inBounds = false;
		if (x>=0 && y>=0 && x<XRES && y<YRES)
		{
			if (elements[t].Properties & TYPE_ENERGY)
				photons[y][x] = PMAP(i, t);
			else
			{
				// Particles are sometimes allowed to go inside INVS and FILT
				// To make particles collide correctly when inside these elements, these elements must not overwrite an existing pmap entry from particles inside them
				if (!pmap[y][x] || (t!=PT_INVIS && t!= PT_FILT))
					pmap[y][x] = PMAP(i, t);
				// (there are a few exceptions, including energy particles - currently no limit on stacking those)
				if (t!=PT_THDR && t!=PT_EMBR && t!=PT_FIGH && t!=PT_PLSM)
					pmap_count[y][x]++;
			}
			inBounds = true;
		}
		NUM_PARTS ++;

		if (elementRecount && t >= 0 && t < PT_NUM && elements[t].Enabled)
			elementCount[t]++;

		//decrease particle life
		if (do_life_dec)
		{
			if (t<0 || t>=PT_NUM || !elements[t].Enabled)
			{
				kill_part(i);
				continue;
			}

			unsigned int elem_properties = elements[t].Properties;
			if (parts[i].life>0 && (elem_properties&PROP_LIFE_DEC) && !(inBounds && bmap[y/CELL][x/CELL] == WL_STASIS && emap[y/CELL][x/CELL]<8))
			{
				// automatically decrease life
				parts[i].life--;
				if (parts[i].life<=0 && (elem_properties&(PROP_LIFE_KILL_DEC|PROP_LIFE_KILL)))
				{
					// kill on change to no life
					kill_part(i);
					continue;
				}
			}
			else if (parts[i].life<=0 && (elem_properties&PROP_LIFE_KILL) && !(inBounds && bmap[y/CELL][x/CELL] == WL_STASIS && emap[y/CELL][x/CELL]<8))
			{
				// kill if no life
				kill_part(i);
				continue;
			}
		}
	}
	parts.Flatten();
	if (elementRecount)
		elementRecount = false;
}

void Parts::Flatten()
{
	int newActive = 0;
	// * Compactacao descarta o encadeamento anterior e reconstroi tudo num unico segmento.
	//   E chamada fora do caminho quente, entao concentrar no segmento 0 e suficiente: os
	//   demais voltam a se encher naturalmente conforme cada thread libera slots.
	pfree.fill(-1);
	auto *ppfree = &pfree[0];
	for (int i = 0; i < active; i++)
	{
		if (data[i].type)
		{
			for (auto j = newActive; j < i; ++j)
			{
				*ppfree = j;
				ppfree = &data[j].life;
			}
			newActive = i + 1;
		}
	}
	*ppfree = -1;
	active = newActive;
}

void Simulation::SimulateGoL()
{
	auto &builtinGol = SimulationData::builtinGol;
	CGOL = 0;
	for (int i = 0; i < parts.active; ++i)
	{
		auto &part = parts[i];
		if (part.type != PT_LIFE)
		{
			continue;
		}
		auto x = int(part.x + 0.5f);
		auto y = int(part.y + 0.5f);
		if (x < CELL || y < CELL || x >= XRES - CELL || y >= YRES - CELL)
		{
			continue;
		}
		unsigned int golnum = part.ctype;
		unsigned int ruleset = golnum;
		if (golnum < NGOL)
		{
			ruleset = builtinGol[golnum].ruleset;
			golnum += 1;
		}
		if (part.tmp2 == int((ruleset >> 17) & 0xF) + 1)
		{
			for (int yy = -1; yy <= 1; ++yy)
			{
				for (int xx = -1; xx <= 1; ++xx)
				{
					if (xx || yy)
					{
						// * Calculate address of the neighbourList, taking wraparound
						//   into account. The fact that the GOL space is 2 CELL's worth
						//   narrower in both dimensions than the simulation area makes
						//   this a bit awkward.
						int ax = ((x + xx + XRES - 3 * CELL) % (XRES - 2 * CELL)) + CELL;
						int ay = ((y + yy + YRES - 3 * CELL) % (YRES - 2 * CELL)) + CELL;
						if (pmap[ay][ax] && TYP(pmap[ay][ax]) != PT_LIFE)
						{
							continue;
						}
						unsigned int (&neighbourList)[5] = gol[ay][ax];
						// * Bump overall neighbour counter (bits 30..28) for the entire list.
						neighbourList[0] += 1U << 28;
						for (int l = 0; l < 5; ++l)
						{
							auto neighbourRuleset = neighbourList[l] & 0x001FFFFFU;
							if (neighbourRuleset == golnum)
							{
								// * Bump population counter (bits 23..21) of the
								//   same kind of cell.
								neighbourList[l] += 1U << 21;
								break;
							}
							if (neighbourRuleset == 0)
							{
								// * Add the new kind of cell to the population. Both counters
								//   have a bias of -1, so they're intentionally initialised
								//   to 0 instead of 1 here. This is all so they can both
								//   fit in 3 bits.
								neighbourList[l] = ((yy & 3) << 26) | ((xx & 3) << 24) | golnum;
								break;
							}
							// * If after 5 iterations the cell still hasn't contributed
							//   to a list entry, it's surely a 6th kind of cell, meaning
							//   there could be at most 3 of it in the neighbourhood,
							//   as there are already 5 other kinds of cells present in
							//   the list. This in turn means that it couldn't possibly
							//   win the population ratio-based contest later on.
						}
					}
				}
			}
		}
		else
		{
			if (!(bmap[y / CELL][x / CELL] == WL_STASIS && emap[y / CELL][x / CELL] < 8))
			{
				part.tmp2 -= 1;
			}
		}
	}
	for (int y = CELL; y < YRES - CELL; ++y)
	{
		for (int x = CELL; x < XRES - CELL; ++x)
		{
			int r = pmap[y][x];
			if (r && TYP(r) != PT_LIFE)
			{
				continue;
			}
			unsigned int (&neighbourList)[5] = gol[y][x];
			auto nl0 = neighbourList[0];
			if (r || nl0)
			{
				// * Get overall neighbour count (bits 30..28).
				unsigned int neighbours = nl0 ? ((nl0 >> 28) & 7) + 1 : 0;
				if (!(bmap[y / CELL][x / CELL] == WL_STASIS && emap[y / CELL][x / CELL] < 8))
				{
					if (r)
					{
						auto &part = parts[ID(r)];
						unsigned int ruleset = part.ctype;
						if (ruleset < NGOL)
						{
							ruleset = builtinGol[ruleset].ruleset;
						}
						if (!((ruleset >> neighbours) & 1) && part.tmp2 == int(ruleset >> 17) + 1)
						{
							// * Start death sequence.
							part.tmp2 -= 1;
						}
					}
					else
					{
						unsigned int golnumToCreate = 0xFFFFFFFFU;
						unsigned int createFromEntry = 0U;
						unsigned int majority = neighbours / 2 + neighbours % 2;
						for (int l = 0; l < 5; ++l)
						{
							auto golnum = neighbourList[l] & 0x001FFFFFU;
							if (!golnum)
							{
								break;
							}
							auto ruleset = golnum;
							if (golnum - 1 < NGOL)
							{
								ruleset = builtinGol[golnum - 1].ruleset;
								golnum -= 1;
							}
							if ((ruleset >> (neighbours + 8)) & 1 && ((neighbourList[l] >> 21) & 7) + 1 >= majority && golnum < golnumToCreate)
							{
								golnumToCreate = golnum;
								createFromEntry = neighbourList[l];
							}
						}
						if (golnumToCreate != 0xFFFFFFFFU)
						{
							// * 0x200000: No need to look for colours, they'll be set later anyway.
							int i = create_part(-1, x, y, PT_LIFE, golnumToCreate | 0x200000);
							if (i >= 0)
							{
								int xx = (createFromEntry >> 24) & 3;
								int yy = (createFromEntry >> 26) & 3;
								if (xx == 3) xx = -1;
								if (yy == 3) yy = -1;
								int ax = ((x - xx + XRES - 3 * CELL) % (XRES - 2 * CELL)) + CELL;
								int ay = ((y - yy + YRES - 3 * CELL) % (YRES - 2 * CELL)) + CELL;
								auto &sample = parts[ID(pmap[ay][ax])];
								parts[i].dcolour = sample.dcolour;
								parts[i].tmp = sample.tmp;
							}
						}
					}
				}
				for (int l = 0; l < 5 && neighbourList[l]; ++l)
				{
					neighbourList[l] = 0;
				}
			}
		}
	}
	for (int y = CELL; y < YRES - CELL; ++y)
	{
		for (int x = CELL; x < XRES - CELL; ++x)
		{
			int r = pmap[y][x];
			if (r && TYP(r) == PT_LIFE && parts[ID(r)].tmp2 <= 0)
			{
				kill_part(ID(r));
			}
		}
	}
}

void Simulation::CheckStacking()
{
	auto &sd = SimulationData::CRef();
	auto &elements = sd.elements;
	bool excessive_stacking_found = false;
	force_stacking_check = false;
	for (int y = 0; y < YRES; y++)
	{
		for (int x = 0; x < XRES; x++)
		{
			// Use a threshold, since some particle stacking can be normal (e.g. BIZR + FILT)
			// Setting pmap_count[y][x] > NPART means BHOL will form in that spot
			if (pmap_count[y][x]>5)
			{
				if (bmap[y/CELL][x/CELL]==WL_EHOLE)
				{
					// Allow more stacking in E-hole
					if (pmap_count[y][x]>1500)
					{
						pmap_count[y][x] = pmap_count[y][x] + NPART;
						excessive_stacking_found = 1;
					}
				}
				else if (pmap_count[y][x]>1500 || (unsigned int)rng.between(0, 1599) <= (pmap_count[y][x]+100))
				{
					pmap_count[y][x] = pmap_count[y][x] + NPART;
					excessive_stacking_found = true;
				}
			}
		}
	}
	if (excessive_stacking_found)
	{
		for (int i = 0; i < parts.active; i++)
		{
			if (parts[i].type)
			{
				int t = parts[i].type;
				int x = (int)(parts[i].x+0.5f);
				int y = (int)(parts[i].y+0.5f);
				if (x>=0 && y>=0 && x<XRES && y<YRES && !(elements[t].Properties&TYPE_ENERGY))
				{
					if (pmap_count[y][x]>=NPART)
					{
						if (pmap_count[y][x]>NPART)
						{
							//@ stacking -> NBHL
							create_part(i, x, y, PT_NBHL);
							parts[i].temp = MAX_TEMP;
							parts[i].tmp = pmap_count[y][x]-NPART;//strength of grav field
							if (parts[i].tmp>51200) parts[i].tmp = 51200;
							pmap_count[y][x] = NPART;
						}
						else
						{
							kill_part(i);
						}
					}
				}
			}
		}
	}
}

void Simulation::UpdateGravityMask()
{
	for (auto p : CELLS.OriginRect())
	{
		gravIn.mask[p] = 0;
	}
	std::stack<Vec2<int>> toCheck;
	auto check = [this, &toCheck](Vec2<int> p) {
		if (!(bmap[p.Y][p.X] == WL_GRAV || gravIn.mask[p]))
		{
			gravIn.mask[p] = UINT32_C(0xFFFFFFFF);
			for (auto o : RectSized<int>({ -1, -1 }, { 3, 3 }))
			{
				if ((o.X + o.Y) & 1) // i.e. immediate neighbours but not diagonal ones
				{
					auto q = p + o;
					if (CELLS.OriginRect().Contains(q))
					{
						toCheck.push(q);
					}
				}
			}
		}
	};
	for (auto x = 0; x < CELLS.X; ++x)
	{
		check({ x, 0           });
		check({ x, CELLS.Y - 1 });
	}
	for (auto y = 1; y < CELLS.Y - 1; ++y) // corners already checked in the previous loop
	{
		check({ 0          , y });
		check({ CELLS.X - 1, y });
	}
	while (!toCheck.empty())
	{
		auto p = toCheck.top();
		toCheck.pop();
		check(p);
	}
}

//updates pmap, gol, and some other simulation stuff (but not particles)
void Simulation::BeforeSim(bool willUpdate)
{
	if (willUpdate)
	{
		{
			FrameTime::Span span(frameTime, "Air::update_air");
			air->update_air();
		}

		if(aheat_enable)
		{
			FrameTime::Span span(frameTime, "Air::update_airh");
			air->update_airh();
		}

		{
			FrameTime::Span span(frameTime, "Simulation::DispatchNewtonianGravity");
			DispatchNewtonianGravity();
		}
		// gravIn::mass is now potentially garbage, which is ok, we were going to clear it for the frame anyway
		for (auto p : gravIn.mass.Size().OriginRect())
		{
			gravIn.mass[p] = 0.f;
		}

		if(emp_decor>0)
			emp_decor -= emp_decor/25+2;
		if(emp_decor < 0)
			emp_decor = 0;
		etrd_count_valid = false;
		etrd_life0_count = 0;

		currentTick++;

		elementRecount |= !(currentTick%180);
		if (elementRecount)
			std::fill(elementCount, elementCount+PT_NUM, 0);
	}
	sandcolour_interface = int(20.0f*sin(float(sandcolour_frame)*std::numbers::pi_v<float>/180.0f));
	sandcolour_frame = (sandcolour_frame+1)%360;
	sandcolour = int(20.0f*sin(float(frameCount)*std::numbers::pi_v<float>/180.0f));

	if (gravWallChanged)
	{
		UpdateGravityMask();
		gravWallChanged = false;
	}

	if (debug_nextToUpdate == 0)
		RecalcFreeParticles(willUpdate);

	if (willUpdate)
	{
		// decrease wall conduction, make walls block air and ambient heat
		for (int y = 0; y < YCELLS; y++)
		{
			for (int x = 0; x < XCELLS; x++)
			{
				if (emap[y][x])
					emap[y][x] --;
				air->bmap_blockair[y][x] = (bmap[y][x]==WL_WALL || bmap[y][x]==WL_WALLELEC || bmap[y][x]==WL_BLOCKAIR || (bmap[y][x]==WL_EWALL && !emap[y][x]));
				air->bmap_blockairh[y][x] = (air->bmap_blockair[y][x] || bmap[y][x]==WL_GRAV) ? 0x8 : 0;
			}
		}

		// check for stacking and create BHOL if found
		if (force_stacking_check || rng.chance(1, 10))
		{
			FrameTime::Span span(frameTime, "Simulation::CheckStacking");
			CheckStacking();
		}

		// LOVE and LOLZ element handling
		if (elementCount[PT_LOVE] > 0 || elementCount[PT_LOLZ] > 0)
		{
			int nx, nnx, ny, nny, r, rt;
			for (ny=0; ny<YRES-4; ny++)
			{
				for (nx=0; nx<XRES-4; nx++)
				{
					r=pmap[ny][nx];
					if (!r)
					{
						continue;
					}
					else if ((ny<9||nx<9||ny>YRES-7||nx>XRES-10)&&(parts[ID(r)].type==PT_LOVE||parts[ID(r)].type==PT_LOLZ))
						kill_part(ID(r));
					else if (parts[ID(r)].type==PT_LOVE)
					{
						Element_LOVE_love[nx/9][ny/9] = 1;
					}
					else if (parts[ID(r)].type==PT_LOLZ)
					{
						Element_LOLZ_lolz[nx/9][ny/9] = 1;
					}
				}
			}
			for (nx=9; nx<=XRES-18; nx++)
			{
				for (ny=9; ny<=YRES-7; ny++)
				{
					if (Element_LOVE_love[nx/9][ny/9]==1)
					{
						for ( nnx=0; nnx<9; nnx++)
							for ( nny=0; nny<9; nny++)
							{
								if (ny+nny>0&&ny+nny<YRES&&nx+nnx>=0&&nx+nnx<XRES)
								{
									rt=pmap[ny+nny][nx+nnx];
									if (!rt&&Element_LOVE_RuleTable[nnx][nny]==1)
										create_part(-1,nx+nnx,ny+nny,PT_LOVE);
									else if (!rt)
										continue;
									else if (parts[ID(rt)].type==PT_LOVE&&Element_LOVE_RuleTable[nnx][nny]==0)
										kill_part(ID(rt));
								}
							}
					}
					Element_LOVE_love[nx/9][ny/9]=0;
					if (Element_LOLZ_lolz[nx/9][ny/9]==1)
					{
						for ( nnx=0; nnx<9; nnx++)
							for ( nny=0; nny<9; nny++)
							{
								if (ny+nny>0&&ny+nny<YRES&&nx+nnx>=0&&nx+nnx<XRES)
								{
									rt=pmap[ny+nny][nx+nnx];
									if (!rt&&Element_LOLZ_RuleTable[nny][nnx]==1)
										create_part(-1,nx+nnx,ny+nny,PT_LOLZ);
									else if (!rt)
										continue;
									else if (parts[ID(rt)].type==PT_LOLZ&&Element_LOLZ_RuleTable[nny][nnx]==0)
										kill_part(ID(rt));

								}
							}
					}
					Element_LOLZ_lolz[nx/9][ny/9]=0;
				}
			}
		}

		// make WIRE work
		if(elementCount[PT_WIRE] > 0)
		{
			for (int nx = 0; nx < XRES; nx++)
			{
				for (int ny = 0; ny < YRES; ny++)
				{
					int r = pmap[ny][nx];
					if (!r)
						continue;
					if(parts[ID(r)].type == PT_WIRE)
						parts[ID(r)].tmp = parts[ID(r)].ctype;
				}
			}
		}

		// update PPIP tmp?
		if (Element_PPIP_ppip_changed)
		{
			for (int i = 0; i < parts.active; i++)
			{
				if (parts[i].type==PT_PPIP)
				{
					parts[i].tmp |= (parts[i].tmp&0xE0000000)>>3;
					parts[i].tmp &= ~0xE0000000;
				}
			}
			Element_PPIP_ppip_changed = 0;
		}

		// Simulate GoL
		// GSPEED is frames per generation
		if (elementCount[PT_LIFE]>0 && ++CGOL>=GSPEED)
		{
			FrameTime::Span span(frameTime, "Simulation::SimulateGoL");
			SimulateGoL();
		}

		// wifi channel reseting
		if (ISWIRE > 0)
		{
			for (int q = 0; q < (int)(MAX_TEMP-73.15f)/100+2; q++)
			{
				wireless[q][0] = wireless[q][1];
				wireless[q][1] = 0;
			}
			ISWIRE--;
		}

		// spawn STKM and STK2
		if (!player.spwn && player.spawnID >= 0)
			create_part(-1, (int)parts[player.spawnID].x, (int)parts[player.spawnID].y, PT_STKM);
		if (!player2.spwn && player2.spawnID >= 0)
			create_part(-1, (int)parts[player2.spawnID].x, (int)parts[player2.spawnID].y, PT_STKM2);

		// particle update happens right after this function (called separately)
	}
}

namespace
{
	// * Checksum determinista do estado da simulacao. Base de qualquer trabalho futuro em
	//   fisica neste fork: sem um valor comparavel entre execucoes nao ha como afirmar que
	//   uma mudanca preservou comportamento, so olhar a tela e torcer.
	//
	//   FNV-1a de 64 bits sobre os bytes crus. A escolha e por ser simples, sem dependencia
	//   e bem definida; nao ha requisito criptografico aqui, so deteccao de divergencia.
	//   Floats entram pelos bits, nao pelo valor, porque o objetivo e justamente pegar
	//   diferenca de ultimo bit vinda de reordenacao de operacoes.
	struct StateChecksum
	{
		uint64_t hash = UINT64_C(1469598103934665603);

		void feedBytes(const void *data, size_t size)
		{
			auto *bytes = static_cast<const unsigned char *>(data);
			for (size_t i = 0; i < size; i++)
			{
				hash ^= bytes[i];
				hash *= UINT64_C(1099511628211);
			}
		}

		template<class T>
		void feed(const T &value)
		{
			feedBytes(&value, sizeof(value));
		}
	};

	std::FILE *checksumFile = nullptr;
	bool checksumChecked = false;
	int checksumFrame = 0;

}

void Simulation::AfterSim()
{
	// * Fixed-schema cost probe output. UpdateParticles only accumulates in memory; doing the
	//   file write here keeps disk I/O outside the measured hot loop.
	DumpParticleCostFrame(this);
	if (!checksumChecked)
	{
		checksumChecked = true;
		if (auto *path = std::getenv("TPT_CHECKSUM_CSV"))
		{
			checksumFile = std::fopen(path, "w");
			if (checksumFile)
			{
				std::fprintf(checksumFile, "frame,live,checksum,freelist_ok,freelist_count,segments,local_alloc,rescue,cls_parallel,cls_serial_type,cls_serial_move,cls_mispredict\n");
			}
		}
	}
	if (checksumFile)
	{
		StateChecksum sum;
		int live = 0;
		for (auto i = 0; i < NPART; i++)
		{
			// * Slots mortos guardam lixo da vida anterior e entram na lista livre, entao
			//   incluí-los mediria o alocador, nao o estado fisico. O indice entra no hash
			//   junto do conteudo para que mover uma particula de slot seja detectado.
			const auto &p = parts[i];
			if (!p.type)
			{
				continue;
			}
			live += 1;
			sum.feed(i);
			sum.feed(p.type);   sum.feed(p.life); sum.feed(p.ctype);
			sum.feed(p.x);      sum.feed(p.y);    sum.feed(p.vx);    sum.feed(p.vy);
			sum.feed(p.temp);   sum.feed(p.flags);
			sum.feed(p.tmp);    sum.feed(p.tmp2); sum.feed(p.tmp3);  sum.feed(p.tmp4);
			sum.feed(p.dcolour);
		}
		// * O grid de ar faz parte do estado: pressao e velocidade realimentam o movimento
		//   no frame seguinte, entao uma divergencia so nele apareceria depois nas particulas.
		sum.feedBytes(pv, sizeof(pv));
		sum.feedBytes(vx, sizeof(vx));
		sum.feedBytes(vy, sizeof(vy));
		sum.feedBytes(hv, sizeof(hv));
		// * Integridade da lista livre segmentada. Corrupcao aqui (slot vivo na lista, slot
		//   em duas listas, ciclo) e silenciosa: nao trava, so faz duas particulas passarem
		//   a compartilhar o mesmo slot. Verificar por frame e caro, mas este caminho ja e
		//   de diagnostico, e o custo so existe com o checksum ligado.
		int freeCount = 0;
		bool freeOk = parts.ValidateFreeLists(freeCount);
		checksumFrame += 1;
		std::fprintf(checksumFile, "%d,%d,%016llx,%d,%d,%d,%lld,%lld,%lld,%lld,%lld,%lld\n", checksumFrame, live,
			(unsigned long long)sum.hash, freeOk ? 1 : 0, freeCount, parts.FreeSegments(),
			parts.localAllocCount, parts.rescueCount,
			clsParallel, clsSerialType, clsSerialMove, clsMispredict);
		std::fflush(checksumFile);

		// * Histograma de mispredicts por elemento, reescrito periodicamente. Reescrever em
		//   vez de acumular linhas mantem o arquivo pequeno e sempre com o total corrente,
		//   sem depender de um gancho de saida limpo que o jogo nao oferece.
		if (!clsMispredictByType.empty() && (checksumFrame % 100) == 0)
		{
			if (auto *path = std::getenv("TPT_MISPREDICT_CSV"))
			{
				if (auto *hist = std::fopen(path, "w"))
				{
					auto &sd = SimulationData::CRef();
					std::fprintf(hist, "elemento,mispredicts,max_dist,em_30px\n");
					for (auto type = 1; type < PT_NUM; type++)
					{
						if (clsMispredictByType[type])
						{
							std::fprintf(hist, "%s,%lld,%.1f,%lld\n",
								sd.elements[type].Name.ToUtf8().c_str(),
								clsMispredictByType[type], clsMispredictMaxDist[type],
								clsMispredictAt30[type]);
						}
					}
					// * Veredito sobre o mecanismo de troca, na mesma linha de saida.
					std::fprintf(hist, "\nmispredicts_com_troca,%lld\n", mispredictWithSwap);
					std::fprintf(hist, "mispredicts_sem_troca,%lld\n", mispredictNoSwap);
					std::fprintf(hist, "trocas_totais,%lld\n", mispredictSwapTotal);
					std::fprintf(hist, "trocas_max_num_frame,%lld\n", mispredictMaxSwaps);
					std::fprintf(hist, "desloc_total_px,%.0f\n", mispredictDistSum);
					std::fprintf(hist, "desloc_explicado_por_troca_px,%.0f\n", mispredictSwapDistSum);
					if (mispredictDistSum > 0.0)
					{
						std::fprintf(hist, "fracao_explicada_por_troca,%.3f\n",
							mispredictSwapDistSum / mispredictDistSum);
					}
					std::fclose(hist);
				}
			}
		}
	}

	debug_mostRecentlyUpdated = -1;

	if (emp_trigger_count)
	{
		// pitiful attempt at trying to keep code relating to a given element in the same file
		Element_EMP_Trigger(this, emp_trigger_count);
		emp_trigger_count = 0;
	}

	frameCount += 1;
}

Simulation::~Simulation()
{
	ReleaseParticleCostOwner(this);
}

Simulation::Simulation()
{
	std::fill(elementCount, elementCount+PT_NUM, 0);
	elementRecount = true;

	//Create and attach air simulation
	air = std::make_unique<Air>(*this);

	player.comm = 0;
	player2.comm = 0;

	clear_sim();

	UpdateGravityMask();
}

void Simulation::DispatchNewtonianGravity()
{
	if (grav)
	{
		grav->Exchange(gravOut, gravIn, gravForceRecalc);
		gravForceRecalc = false;
	}
}

void Simulation::ResetNewtonianGravity(GravityInput newGravIn, GravityOutput newGravOut)
{
	gravIn = newGravIn;
	DispatchNewtonianGravity();
	// gravIn is now potentially garbage, set it again
	gravIn = newGravIn;
	if (grav)
	{
		gravOut = newGravOut;
		gravForceRecalc = true; // gravOut changed outside DispatchNewtonianGravity
		gravWallChanged = true;
	}
}

void Simulation::EnableNewtonianGravity(bool enable)
{
	if (grav && !enable)
	{
		grav.reset();
		gravOut = {}; // reset as per the invariant
		gravForceRecalc = true; // gravOut changed outside DispatchNewtonianGravity
	}
	if (!grav && enable)
	{
		grav = Gravity::Create();
		auto oldGravIn = gravIn;
		DispatchNewtonianGravity();
		// gravIn is now potentially garbage, set it again
		gravIn = std::move(oldGravIn);
	}
}

// we want XRES * YRES <= (1 << (31 - PMAPBITS)), but we do a division because multiplication could silently overflow
static_assert(uint32_t(XRES) <= (UINT32_C(1) << (31 - PMAPBITS)) / uint32_t(YRES), "not enough space in pmap");
