using Docker.DotNet;
using Docker.DotNet.Models;
using Playerbot.Web.Abstractions;
using Playerbot.Web.Models;

namespace Playerbot.Web.Services;

public class RemoteEditSessionManager : IRemoteEditSessionManager
{
    private readonly DockerClient _client = new DockerClientConfiguration()
        .CreateClient();

    public async Task<IEnumerable<RemoteEditSession>> GetSessionsAsync()
    {
        var containers = await _client.Containers.ListContainersAsync(
            new ContainersListParameters
            {
                Limit = 10
            });

        return containers.Select(c => new RemoteEditSession
        {
            Name = c.Names.First(),
            Id = c.ID
        });
    }

    public async Task<RemoteEditSession> StartSessionAsync(int accountId)
    {
        var name = $"remote-{accountId}";
        var response = await _client.Containers.CreateContainerAsync(new CreateContainerParameters
        {
            Name = name,
            ExposedPorts = new Dictionary<string, EmptyStruct> { {"8000", new EmptyStruct {}}},
            Env = null,
            Healthcheck = null,
            Image = "playerbot-remote",
            Volumes = null,
            WorkingDir = null,
        });

        return new RemoteEditSession { Name = name, AccountId = accountId, Id = response.ID };
    }

    public Task StopSessionAsync(RemoteEditSession session)
    {
        throw new NotImplementedException();
    }
}
