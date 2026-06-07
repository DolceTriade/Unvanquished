#include "ClientComponent.h"

#include "common/Common.h"

ClientComponent::ClientComponent( Entity &entity, gclient_t *clientData,
                                  TeamComponent &r_TeamComponent )
	: ClientComponentBase( entity, clientData, r_TeamComponent )
{}

void ClientComponent::HandleDie( gentity_t*, meansOfDeath_t )
{
	G_ClearClientBuildQueue( this->entity.oldEnt, false );
}
