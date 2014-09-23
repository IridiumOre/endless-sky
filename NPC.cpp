/* NPC.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.
*/

#include "NPC.h"

#include "ConversationPanel.h"
#include "DataNode.h"
#include "DataWriter.h"
#include "Dialog.h"
#include "Format.h"
#include "GameData.h"
#include "Messages.h"
#include "Random.h"
#include "ShipEvent.h"
#include "UI.h"

#include <vector>

using namespace std;



void NPC::Load(const DataNode &node)
{
	// Any tokens after the "npc" tag list the things that must happen for this
	// mission to succeed.
	for(int i = 1; i < node.Size(); ++i)
	{
		if(node.Token(i) == "save")
			failIf |= ShipEvent::DESTROY;
		if(node.Token(i) == "kill")
			succeedIf |= ShipEvent::DESTROY;
		if(node.Token(i) == "board")
			succeedIf |= ShipEvent::BOARD;
		if(node.Token(i) == "disable")
			succeedIf |= ShipEvent::DISABLE;
		if(node.Token(i) == "scan cargo")
			succeedIf |= ShipEvent::SCAN_CARGO;
		if(node.Token(i) == "scan outfits")
			succeedIf |= ShipEvent::SCAN_OUTFITS;
	}
	
	for(const DataNode &child : node)
	{
		bool doEnter = child.Token(0) == "enter";
		bool doRemain = child.Token(0) == "remain";
		bool doWait = child.Token(0) == "wait";
		if(doEnter || doRemain || doWait)
		{
			if(child.Size() >= 2)
				system = GameData::Systems().Get(child.Token(1));
			else
				location.Load(child);
		}
		else if(child.Token(0) == "succeed" && child.Size() >= 2)
			succeedIf = child.Value(1);
		else if(child.Token(0) == "fail" && child.Size() >= 2)
			failIf = child.Value(1);
		else if(child.Token(0) == "government" && child.Size() >= 2)
			government = GameData::Governments().Get(child.Token(1));
		else if(child.Token(0) == "dialog")
		{
			for(int i = 1; i < child.Size(); ++i)
			{
				if(!dialogText.empty())
					dialogText += "\n\t";
				dialogText += child.Token(i);
			}
			for(const DataNode &grand : child)
				for(int i = 0; i < grand.Size(); ++i)
				{
					if(!dialogText.empty())
						dialogText += "\n\t";
					dialogText += grand.Token(i);
				}
		}
		else if(child.Token(0) == "conversation" && child.HasChildren())
			conversation.Load(child);
		else if(child.Token(0) == "conversation" && child.Size() > 1)
			stockConversation = GameData::Conversations().Get(child.Token(1));
		else if(child.Token(0) == "ship")
		{
			if(child.HasChildren())
			{
				ships.push_back(make_shared<Ship>());
				ships.back()->Load(child);
				for(const DataNode &grand : child)
					if(grand.Token(0) == "actions" && grand.Size() >= 2)
						actions[ships.back().get()] = grand.Value(1);
			}
			else if(child.Size() >= 2)
			{
				stockShips.push_back(GameData::Ships().Get(child.Token(1)));
				shipNames.push_back(child.Token(1 + (child.Size() > 2)));
			}
		}
		else if(child.Token(0) == "fleet")
		{
			if(child.HasChildren())
			{
				fleets.push_back(Fleet());
				fleets.back().Load(child);
			}
			else if(child.Size() >= 2)
				stockFleets.push_back(GameData::Fleets().Get(child.Token(1)));
		}
	}
	
	// Since a ship's government is not serialized, set it now.
	for(const shared_ptr<Ship> &ship : ships)
	{
		ship->SetGovernment(government);
		ship->SetIsSpecial();
		ship->FinishLoading();
	}
}



// Note: the Save() function can assume this is an instantiated mission, not
// a template, so fleets will be replaced by individual ships already.
void NPC::Save(DataWriter &out) const
{
	out.Write("npc");
	out.BeginChild();
	
	if(succeedIf)
		out.Write("succeed", succeedIf);
	if(failIf)
		out.Write("fail", failIf);
	
	if(government)
		out.Write("government", government->GetName());
	
	if(!dialogText.empty())
	{
		out.Write("dialog");
		out.BeginChild();
		
		// Break the text up into paragraphs.
		size_t begin = 0;
		while(true)
		{
			size_t pos = dialogText.find("\n\t", begin);
			if(pos == string::npos)
				pos = dialogText.length();
			out.Write(dialogText.substr(begin, pos - begin));
			if(pos == dialogText.length())
				break;
			begin = pos + 2;
		}
		out.EndChild();
	}
	if(!conversation.IsEmpty())
		conversation.Save(out);
	
	for(const shared_ptr<Ship> &ship : ships)
	{
		ship->Save(out);
		auto it = actions.find(ship.get());
		if(it != actions.end() && it->second)
		{
			// Append an "actions" tag to the end of the ship data.
			out.BeginChild();
			out.Write("actions", it->second);
			out.EndChild();
		}
	}
	
	out.EndChild();
}



// Get the ships associated with this set of NPCs.
const list<shared_ptr<Ship>> NPC::Ships() const
{
	return ships;
}



// Handle the given ShipEvent.
void NPC::Do(const ShipEvent &event, PlayerInfo &player, UI *ui)
{
	bool hasSucceeded = HasSucceeded();
	bool hasFailed = HasFailed();
	for(const shared_ptr<Ship> &ship : ships)
		if(ship == event.Target())
		{
			actions[ship.get()] |= event.Type();
			break;
		}
	
	if(HasFailed() && !hasFailed)
		Messages::Add("Mission failed.");
	else if(ui && HasSucceeded() && !hasSucceeded)
	{
		if(!conversation.IsEmpty())
			ui->Push(new ConversationPanel(player, conversation));
		else if(!dialogText.empty())
			ui->Push(new Dialog(dialogText));
	}
}



bool NPC::HasSucceeded() const
{
	if(HasFailed())
		return false;
	
	if(!succeedIf)
		return true;
	
	for(const shared_ptr<Ship> &ship : ships)
	{
		auto it = actions.find(ship.get());
		if(it == actions.end() || (it->second & succeedIf) != succeedIf)
			return false;
	}
	
	return true;
}



bool NPC::HasFailed() const
{
	for(const auto &it : actions)
		if(it.second & failIf)
			return true;
	
	return false;
}



// Create a copy of this NPC but with the fleets replaced by the actual
// ships they represent, wildcards in the conversation text replaced, etc.
NPC NPC::Instantiate(map<string, string> &subs, const System *origin) const
{
	NPC result;
	result.government = government;
	if(!result.government)
		result.government = GameData::PlayerGovernment();
	result.succeedIf = succeedIf;
	result.failIf = failIf;
	
	// Pick the system for this NPC to start out in.
	result.system = system;
	if(!result.system && !location.IsEmpty())
	{
		// Find a destination that satisfies the filter.
		vector<const System *> options;
		for(const auto &it : GameData::Systems())
		{
			// Skip entries with incomplete data.
			if(it.second.Name().empty())
				continue;
			if(location.Matches(&it.second, origin))
				options.push_back(&it.second);
		}
		if(!options.empty())
			result.system = options[Random::Int(options.size())];
	}
	if(!result.system)
		result.system = origin;
	
	// Convert fleets into instances of ships.
	for(const shared_ptr<Ship> &ship : ships)
	{
		result.ships.push_back(make_shared<Ship>(*ship));
		result.ships.back()->FinishLoading();
	}
	auto shipIt = stockShips.begin();
	auto nameIt = shipNames.begin();
	for( ; shipIt != stockShips.end() && nameIt != shipNames.end(); ++shipIt, ++nameIt)
	{
		result.ships.push_back(make_shared<Ship>(**shipIt));
		result.ships.back()->SetName(*nameIt);
	}
	for(const shared_ptr<Ship> &ship : result.ships)
	{
		Angle angle = Angle::Random();
		Point pos = Angle::Random().Unit() * Random::Real() * 400.;
		double velocity = Random::Real() * ship->MaxVelocity();
		
		ship->SetSystem(result.system);
		ship->Place(pos, velocity * angle.Unit(), angle);
	}
	for(const Fleet &fleet : fleets)
		fleet.Place(*result.system, result.ships);
	for(const Fleet *fleet : stockFleets)
		fleet->Place(*result.system, result.ships);
	
	for(const shared_ptr<Ship> &ship : result.ships)
	{
		ship->SetGovernment(government);
		ship->SetIsSpecial();
	}
	
	// String replacement:
	if(!ships.empty())
		subs["<npc>"] = ships.front()->Name();
	
	// Do string replacement on any dialog or conversation.
	if(!dialogText.empty())
		result.dialogText = Format::Replace(dialogText, subs);
	
	if(stockConversation)
		result.conversation = stockConversation->Substitute(subs);
	else if(!conversation.IsEmpty())
		result.conversation = conversation.Substitute(subs);
	
	return result;
}