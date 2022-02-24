namespace Playerbot.Api.Models;

public class PlayerbotScriptsDto
{
    public int AccountId { get; set; }
    public IEnumerable<PlayerbotScript> Scripts { get; set; } = new List<PlayerbotScript>();
    public bool IsComplete { get; set; } = true;
}
