#include <base/math.h>

#include <engine/shared/config.h>

#include <game/gamecore.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <game/server/entities/character.h>

#include "rollback.h"

CGameWorld *CRollback::GameWorld()
{
	return &m_pGameServer->m_World;
}

CGameContext *CRollback::GameServer()
{
	return m_pGameServer;
}

IServer *CRollback::Server()
{
	return m_pGameServer->Server();
}

CCollision *CRollback::Collision()
{
	return m_pGameServer->Collision();
}

CTuningParams *CRollback::Tuning()
{
	return m_pGameServer->Tuning();
}

CTuningParams *CRollback::TuningList()
{
	return m_pGameServer->TuningList();
}

void CRollback::Init(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
}

inline int CRollback::NormalizeTick(int Tick)
{
	if(Tick < 0)
		Tick = Server()->Tick();
	else if(Tick + ROLLBACK_POSITION_HISTORY < Server()->Tick())
		Tick = Server()->Tick() - ROLLBACK_POSITION_HISTORY;

	return Tick % ROLLBACK_POSITION_HISTORY;
}

CCharacter *CRollback::IntersectCharacterOnTick(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, const CCharacter *pNotThis, int CollideWith, const CCharacter *pThisOnly, const CCharacter *pOwnerChar, int Tick)
{
	float ClosestLen = distance(Pos0, Pos1) * 100.0f;
	CCharacter *pClosest = nullptr;
	int LocalTick = NormalizeTick(Tick);

	CCharacter *pChar = (CCharacter *)GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER);
	for(; pChar; pChar = (CCharacter *)pChar->TypeNext())
	{
		if(pChar == pNotThis)
			continue;

		if(pThisOnly && pChar != pThisOnly)
			continue;

		if(CollideWith != -1 && !pChar->CanCollide(CollideWith))
			continue;

		vec2 Charpos;
		if(pChar != pOwnerChar && pChar->m_Positions[LocalTick].m_Valid)
			Charpos = pChar->m_Positions[LocalTick].m_Position;
		else
			Charpos = pChar->m_Pos;

		vec2 IntersectPos;
		if(closest_point_on_line(Pos0, Pos1, Charpos, IntersectPos))
		{
			float Len = distance(Charpos, IntersectPos);
			if(Len < pChar->GetProximityRadius() + Radius)
			{
				Len = distance(Pos0, IntersectPos);
				if(Len < ClosestLen)
				{
					NewPos = IntersectPos;
					ClosestLen = Len;
					pClosest = pChar;
				}
			}
		}
	}

	return pClosest;
}

int CRollback::FindCharactersOnTick(vec2 Pos, float Radius, CEntity **ppEnts, int Max, int Tick)
{
	int LocalTick = NormalizeTick(Tick);

	int Num = 0;
	for(CCharacter *pChr = (CCharacter *)GameWorld()->FindFirst(CGameWorld::ENTTYPE_CHARACTER); pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr->m_Positions[LocalTick].m_Valid ? (distance(pChr->m_Positions[LocalTick].m_Position, Pos) < Radius + pChr->GetProximityRadius()) : (distance(pChr->m_Pos, Pos) < Radius + pChr->GetProximityRadius()))
		{
			if(ppEnts)
				ppEnts[Num] = pChr;
			Num++;
			if(Num == Max)
				break;
		}
	}

	return Num;
}

void CRollback::CreateExplosionOnTick(vec2 Pos, int Owner, int Weapon, bool NoDamage, int ActivatedTeam, int Tick, CClientMask Mask, CClientMask SprayMask)
{
	int LocalTick = NormalizeTick(Tick);

	// create the event
	CNetEvent_Explosion *pEvent = GameServer()->m_Events.Create<CNetEvent_Explosion>(Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	// deal damage
	CEntity *apEnts[MAX_CLIENTS];
	float Radius = 135.0f;
	float InnerRadius = 48.0f;
	int Num = GameWorld()->FindEntities(Pos, Radius, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	CClientMask TeamMask = CClientMask().set();
	for(int i = 0; i < Num; i++)
	{
		auto *pChr = static_cast<CCharacter *>(apEnts[i]);
		vec2 Diff;
		if(pChr->m_Positions[LocalTick].m_Valid && pChr->GetPlayer() && pChr->GetPlayer()->GetCid() != Owner) //+KZ treat owner position as normal
			Diff = pChr->m_Positions[LocalTick].m_Position - Pos;
		else
			Diff = pChr->m_Pos - Pos;
		vec2 ForceDir(0, 1);
		float l = length(Diff);
		if(l)
			ForceDir = normalize(Diff);
		l = 1 - std::clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		float Strength;
		if(Owner == -1 || !GameServer()->m_apPlayers[Owner] || !GameServer()->m_apPlayers[Owner]->m_TuneZone)
			Strength = Tuning()->m_ExplosionStrength;
		else
			Strength = TuningList()[GameServer()->m_apPlayers[Owner]->m_TuneZone].m_ExplosionStrength;

		float Dmg = Strength * l;
		if(!(int)Dmg)
			continue;

		if((GameServer()->GetPlayerChar(Owner) ? !GameServer()->GetPlayerChar(Owner)->GrenadeHitDisabled() : g_Config.m_SvHit) || NoDamage || Owner == pChr->GetPlayer()->GetCid())
		{
			if(Owner != -1 && pChr->IsAlive() && !pChr->CanCollide(Owner))
				continue;
			if(Owner == -1 && ActivatedTeam != -1 && pChr->IsAlive() && pChr->Team() != ActivatedTeam)
				continue;

			// Explode at most once per team
			int PlayerTeam = pChr->Team();
			if((GameServer()->GetPlayerChar(Owner) ? GameServer()->GetPlayerChar(Owner)->GrenadeHitDisabled() : !g_Config.m_SvHit) || NoDamage)
			{
				if(PlayerTeam == TEAM_SUPER)
					continue;
				if(!TeamMask.test(PlayerTeam))
					continue;
				TeamMask.reset(PlayerTeam);
			}

			pChr->TakeDamage(ForceDir * Dmg * 2, (int)Dmg, Owner, Weapon);
		}
	}
}
